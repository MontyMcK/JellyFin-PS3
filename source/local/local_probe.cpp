// local_probe: see local_probe.h.

#include "local_probe.h"
#include "i18n.h"
#include "lang_names.h"
#include "mkv_demux.h"
#include "mkv_ts.h"
#include "h264_sps.h"
#include "stream_local.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TS_PKT 188
#define M2TS_PKT 192
#define SCAN_BYTES (8u * 1024u * 1024u)       // how far into a TS file the PMT and the first SPS are looked for

static void set_err(char *err, int cap, const char *m) { if (err && cap > 0) snprintf(err, (size_t)cap, "%s", m); }

// ---- m2ts -----------------------------------------------------------------------------------

uint64_t local_m2ts_ts_size(uint64_t file_size) { return file_size / M2TS_PKT * TS_PKT; }

int local_m2ts_read_at(void *view, uint64_t off, uint8_t *buf, int len) {
    LocalM2tsView *v = (LocalM2tsView *)view;
    static uint8_t raw[348 * M2TS_PKT];
    int done = 0;
    while (done < len) {
        const uint64_t pos = off + (uint64_t)done;
        const uint64_t first = pos / TS_PKT;
        const int inner = (int)(pos % TS_PKT);
        int pkts = (inner + (len - done) + TS_PKT - 1) / TS_PKT;
        if (pkts > 348) pkts = 348;
        const int got = v->rd(v->ctx, first * M2TS_PKT, raw, pkts * M2TS_PKT);
        if (got < 0) return done ? done : got;
        const int whole = got / M2TS_PKT;
        if (whole == 0) break;
        int copied = 0;
        for (int i = 0; i < whole && done + copied < len; i++) {
            const int from = i == 0 ? inner : 0;
            int n = TS_PKT - from;
            if (n > len - done - copied) n = len - done - copied;
            memcpy(buf + done + copied, raw + (size_t)i * M2TS_PKT + 4 + from, (size_t)n);
            copied += n;
        }
        done += copied;
        if (whole < pkts) break;                         // the file ends here
    }
    return done;
}

int local_m2ts_compact(uint8_t *buf, int len) {
    const int whole = len / M2TS_PKT;
    for (int i = 0; i < whole; i++) memmove(buf + (size_t)i * TS_PKT, buf + (size_t)i * M2TS_PKT + 4, TS_PKT);
    return whole * TS_PKT;
}

// ---- names ---------------------------------------------------------------------------------

static const char *audio_codec_name(LocalAudioCodec c, bool hd_ma) {
    switch (c) {
    case LA_AC3:    return "Dolby Digital";
    case LA_EAC3:   return "Dolby Digital Plus";
    case LA_DTS:    return "DTS";
    case LA_DTS_HD: return hd_ma ? "DTS-HD MA" : "DTS-HD";
    case LA_TRUEHD: return "Dolby TrueHD";
    case LA_MP3:    return "MP3";
    case LA_MP2:    return "MP2";
    case LA_AAC:    return "AAC";
    case LA_FLAC:   return "FLAC";
    case LA_PCM:    return "PCM";
    case LA_VORBIS: return "Vorbis";
    case LA_OPUS:   return "Opus";
    default:        return "Audio";
    }
}

static const char *channel_name(int ch, char *buf, size_t cap) {
    switch (ch) {
    case 0: return "";
    case 1: return "Mono";
    case 2: return "Stereo";
    case 6: return "5.1";
    case 8: return "7.1";
    default: snprintf(buf, cap, "%d ch", ch); return buf;
    }
}

int local_clip_utf8(const char *s, int max) {
    int n = 0;
    while (n < max && s[n]) n++;
    if (n == max && s[n]) {                               // cut short: not in the middle of a sequence
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    }
    return n;
}

static void make_audio_label(LocalAudio *a, bool hd_ma, const char *name) {
    char lang[32], chb[16];
    lang_name(a->lang, lang, sizeof lang);
    const char *ch = channel_name(a->channels, chb, sizeof chb);
    int n = snprintf(a->label, sizeof a->label, "%s - %s", lang, audio_codec_name(a->codec, hd_ma));
    if (ch[0] && n < (int)sizeof a->label) n += snprintf(a->label + n, sizeof a->label - (size_t)n, " - %s", ch);
    if (a->is_default && n < (int)sizeof a->label) n += snprintf(a->label + n, sizeof a->label - (size_t)n, " - Default");
    if (name && name[0] && strcasecmp(name, lang) != 0 && n < (int)sizeof a->label) {
        // the name last, cut to what is left of the label without splitting a character
        const int room = (int)sizeof a->label - n - 4;                  // " - " and the terminator
        const int take = room > 0 ? local_clip_utf8(name, room < 24 ? room : 24) : 0;
        if (take > 0) snprintf(a->label + n, sizeof a->label - (size_t)n, " - %.*s", take, name);
    }
}

static bool has_word(const char *s, const char *w) {
    const size_t wl = strlen(w);
    for (; *s; s++) if (strncasecmp(s, w, wl) == 0) return true;
    return false;
}

static void make_video_desc(LocalInfo *o, const char *codec, const H264SpsInfo *sps) {
    char fps[24] = "";
    if (o->fps > 0) snprintf(fps, sizeof fps, " %.3f fps", o->fps);
    char *e = o->video_desc;
    size_t cap = sizeof o->video_desc;
    int n = snprintf(e, cap, "%s", codec);
    if (sps && n < (int)cap) {
        const char *prof = sps->profile_idc == 100 ? " High" : sps->profile_idc == 77 ? " Main" : sps->profile_idc == 66 ? " Baseline" : "";
        n += snprintf(e + n, cap - (size_t)n, "%s", prof);
    }
    if (o->width > 0 && n < (int)cap) n += snprintf(e + n, cap - (size_t)n, " %dx%d", o->width, o->height);
    if (fps[0] && n < (int)cap) snprintf(e + n, cap - (size_t)n, "%s", fps);
    // trim a trailing ".000" of a whole number rate
    char *dot = strstr(o->video_desc, ".000 fps");
    if (dot) memmove(dot, dot + 4, strlen(dot + 4) + 1);
}

// ---- MKV -------------------------------------------------------------------------------------

static LocalAudioCodec from_mkv_audio(MkvAudioCodec c) {
    switch (c) {
    case MKV_AC_AC3: return LA_AC3;   case MKV_AC_EAC3: return LA_EAC3; case MKV_AC_DTS: return LA_DTS;
    case MKV_AC_TRUEHD: return LA_TRUEHD; case MKV_AC_MP3: return LA_MP3; case MKV_AC_MP2: return LA_MP2;
    case MKV_AC_AAC: return LA_AAC;   case MKV_AC_FLAC: return LA_FLAC; case MKV_AC_PCM: return LA_PCM;
    case MKV_AC_VORBIS: return LA_VORBIS; case MKV_AC_OPUS: return LA_OPUS;
    default: return LA_OTHER;
    }
}

static bool probe_mkv(LocalReadAt rd, void *ctx, uint64_t size, LocalInfo *o, char *err, int cap) {
    // the parsed file is ~60 KB: kept off the stack, since a worker thread's is 128 KB
    MkvFile *fp = (MkvFile *)malloc(sizeof *fp);
    if (!fp) { set_err(err, cap, TR("out of memory")); return false; }
    MkvFile &f = *fp;
    if (mkv_open(&f, rd, ctx, size) != 0) { set_err(err, cap, f.err[0] ? f.err : TR("not a Matroska file")); free(fp); return false; }
    o->container = LM_MKV;
    o->duration_secs = (uint32_t)(f.duration_ns / 1e9 + 0.5);

    const MkvTrack *v = mkv_first_track(&f, MKV_TRACK_VIDEO);
    if (!v) {
        o->video_ok = false;
        snprintf(o->video_reason, sizeof o->video_reason, "no video");
        snprintf(o->video_desc, sizeof o->video_desc, TR("No video"));
    } else {
        o->video_id = v->number; o->width = v->width; o->height = v->height;
        if (v->default_duration_ns) o->fps = 1e9 / (double)v->default_duration_ns;
        char why[48];
        o->video_ok = mkv_ts_video_supported(&f, v->number, why, sizeof why);
        snprintf(o->video_reason, sizeof o->video_reason, "%s", why);
        const char *codec = "Video";
        H264SpsInfo sps; bool have = false;
        switch (mkv_video_codec(v)) {
        case MKV_VC_AVC: {
            codec = "H.264";
            MkvAvcConfig avc;
            if (mkv_parse_avcc(v->codec_private, v->cp_len, &avc) && h264_parse_sps(avc.sps[0], avc.sps_len[0], &sps)) {
                have = true;
                if (sps.width > 0) { o->width = sps.width; o->height = sps.height; }
                if (sps.has_timing && sps.fps > 0 && sps.fps < 130) o->fps = sps.fps;
                if (o->video_ok) {
                    char r2[48];
                    if (!h264_playable(&sps, r2, sizeof r2)) { o->video_ok = false; snprintf(o->video_reason, sizeof o->video_reason, "%s", r2); }
                }
            }
            break;
        }
        case MKV_VC_HEVC: codec = "HEVC"; break;
        case MKV_VC_VC1: codec = "VC-1"; break;
        case MKV_VC_MPEG2: codec = "MPEG-2"; break;
        case MKV_VC_VP9: codec = "VP9"; break;
        case MKV_VC_AV1: codec = "AV1"; break;
        default: break;
        }
        make_video_desc(o, codec, have ? &sps : NULL);
    }

    for (int i = 0; i < f.n_tracks; i++) {
        const MkvTrack *t = &f.tracks[i];
        if (!t->enabled) continue;
        if (t->type == MKV_TRACK_AUDIO && o->n_audio < LOCAL_MAX_TRACKS) {
            LocalAudio *a = &o->audio[o->n_audio++];
            memset(a, 0, sizeof *a);
            a->id = t->number;
            snprintf(a->lang, sizeof a->lang, "%s", t->language);
            const MkvAudioCodec mc = mkv_audio_codec(t);
            a->codec = from_mkv_audio(mc);
            const bool ma = strstr(t->codec_id, "LOSSLESS") != NULL || strstr(t->codec_id, "/MA") != NULL;
            if (a->codec == LA_DTS && ma) a->codec = LA_DTS_HD;
            a->decodable = mkv_ts_audio_track_supported(t, NULL, 0);
            a->is_default = t->is_default;
            a->commentary = has_word(t->name, "commentary");
            a->channels = t->channels;
            make_audio_label(a, ma, t->name);
        } else if (t->type == MKV_TRACK_SUBTITLE && o->n_subs < LOCAL_MAX_TRACKS) {
            LocalSub *s = &o->subs[o->n_subs++];
            memset(s, 0, sizeof *s);
            s->id = t->number;
            snprintf(s->lang, sizeof s->lang, "%s", t->language);
            switch (mkv_sub_codec(t)) {
            case MKV_SC_SRT: s->kind = LS_SRT; break;
            case MKV_SC_ASS: s->kind = LS_ASS; break;
            case MKV_SC_PGS: s->kind = LS_PGS; break;
            default: s->kind = LS_OTHER; break;
            }
            s->usable = s->kind != LS_OTHER && !t->unsupported_encoding;
            s->forced = t->is_forced; s->is_default = t->is_default;
            char lang[32];
            lang_name(t->language, lang, sizeof lang);
            snprintf(s->label, sizeof s->label, "%.*s%s%s%.*s", local_clip_utf8(lang, 30), lang,
                     s->forced ? " - Forced" : "", t->name[0] ? " - " : "", local_clip_utf8(t->name, 24), t->name);
        }
    }
    mkv_close(&f);
    free(fp);
    return true;
}

// ---- TS --------------------------------------------------------------------------------------

struct Psi { uint8_t buf[4096]; int len; bool active; int need; };

// Collects a PSI section across packets.  True once a whole section is in `p->buf`.
static bool psi_feed(Psi *p, const uint8_t *pkt) {
    const bool pusi = (pkt[1] & 0x40) != 0;
    const int afl = (pkt[3] >> 4) & 3;
    if (!(afl & 1)) return false;
    int off = 4;
    if (afl & 2) off += pkt[4] + 1;
    if (off >= TS_PKT) return false;
    const uint8_t *pay = pkt + off;
    int plen = TS_PKT - off;
    if (pusi) {
        const int ptr = pay[0];
        if (1 + ptr >= plen) return false;
        pay += 1 + ptr; plen -= 1 + ptr;
        p->len = 0; p->active = true; p->need = 0;
    } else if (!p->active) return false;
    if (p->len + plen > (int)sizeof p->buf) { p->active = false; return false; }
    memcpy(p->buf + p->len, pay, (size_t)plen);
    p->len += plen;
    if (p->len >= 3 && p->need == 0) p->need = 3 + (((p->buf[1] & 0x0F) << 8) | p->buf[2]);
    if (p->need && p->len >= p->need) { p->active = false; return true; }
    return false;
}

struct TsStream { int stype; int pid; char lang[8]; bool ac3_desc, eac3_desc, dts_desc; };

static void collect_desc(TsStream *s, const uint8_t *d, int len) {
    int pos = 0;
    while (pos + 2 <= len) {
        const int tag = d[pos], dl = d[pos + 1];
        if (pos + 2 + dl > len) break;
        if (tag == 0x0A && dl >= 3) { memcpy(s->lang, d + pos + 2, 3); s->lang[3] = '\0'; }
        else if (tag == 0x6A) s->ac3_desc = true;
        else if (tag == 0x7A) s->eac3_desc = true;
        else if (tag == 0x7B) s->dts_desc = true;
        else if (tag == 0x05 && dl >= 4) {
            if (!memcmp(d + pos + 2, "AC-3", 4)) s->ac3_desc = true;
            else if (!memcmp(d + pos + 2, "EAC3", 4)) s->eac3_desc = true;
            else if (!memcmp(d + pos + 2, "DTS", 3)) s->dts_desc = true;
        }
        pos += 2 + dl;
    }
}

static bool probe_ts(LocalReadAt rd, void *ctx, uint64_t size, bool m2ts, LocalInfo *o, char *err, int cap) {
    LocalM2tsView view = { rd, ctx };
    LocalReadAt rdr = m2ts ? local_m2ts_read_at : rd;
    void *rctx = m2ts ? (void *)&view : ctx;
    const uint64_t ts_size = m2ts ? local_m2ts_ts_size(size) : size;
    o->container = m2ts ? LM_M2TS : LM_TS;

    // the read window and the tables are ~100 KB: off the stack, since a worker thread's is 128 KB
    struct Bufs { uint8_t chunk[TS_PKT * 348]; Psi pat, pmt; uint8_t vbuf[24 * 1024]; };
    Bufs *bufs = (Bufs *)calloc(1, sizeof *bufs);
    if (!bufs) { set_err(err, cap, TR("out of memory")); return false; }
    uint8_t *chunk = bufs->chunk;
    Psi &pat = bufs->pat, &pmt = bufs->pmt;
    uint8_t (&vbuf)[24 * 1024] = bufs->vbuf;
    int pmt_pid = -1;
    TsStream st[24]; int nst = 0;
    bool have_pmt = false;
    int vlen = 0; int video_pid = -1; bool vstarted = false;
    uint64_t pos = 0;
    // find the first sync byte (a file may begin mid-packet)
    {
        uint8_t head[TS_PKT * 3];
        const int n = rdr(rctx, 0, head, (int)sizeof head);
        int s = -1;
        for (int i = 0; i < TS_PKT && i + 2 * TS_PKT < n; i++)
            if (head[i] == 0x47 && head[i + TS_PKT] == 0x47 && head[i + 2 * TS_PKT] == 0x47) { s = i; break; }
        if (s < 0) { free(bufs); set_err(err, cap, TR("not a transport stream")); return false; }
        pos = (uint64_t)s;
    }
    while (pos < ts_size && pos < SCAN_BYTES && !(have_pmt && vlen >= (int)sizeof vbuf)) {
        const int want = (int)(ts_size - pos < (uint64_t)TS_PKT * 348 ? ts_size - pos : (uint64_t)TS_PKT * 348);
        const int n = rdr(rctx, pos, chunk, want);
        if (n < TS_PKT) break;
        for (int off = 0; off + TS_PKT <= n; off += TS_PKT) {
            const uint8_t *p = chunk + off;
            if (p[0] != 0x47) continue;
            const int pid = ((p[1] & 0x1F) << 8) | p[2];
            if (pid == 0 && pmt_pid < 0) {
                if (psi_feed(&pat, p) && pat.buf[0] == 0x00) {
                    const int end = pat.need - 4;
                    for (int q = 8; q + 4 <= end; q += 4) {
                        const int prog = (pat.buf[q] << 8) | pat.buf[q + 1];
                        if (prog != 0) { pmt_pid = ((pat.buf[q + 2] & 0x1F) << 8) | pat.buf[q + 3]; break; }
                    }
                }
            } else if (pid == pmt_pid && !have_pmt) {
                if (psi_feed(&pmt, p) && pmt.buf[0] == 0x02) {
                    const int end = pmt.need - 4;
                    int q = 12 + (((pmt.buf[10] & 0x0F) << 8) | pmt.buf[11]);
                    while (q + 5 <= end && nst < 24) {
                        TsStream *s = &st[nst];
                        memset(s, 0, sizeof *s);
                        s->stype = pmt.buf[q];
                        s->pid = ((pmt.buf[q + 1] & 0x1F) << 8) | pmt.buf[q + 2];
                        const int il = ((pmt.buf[q + 3] & 0x0F) << 8) | pmt.buf[q + 4];
                        if (q + 5 + il > end) break;
                        collect_desc(s, pmt.buf + q + 5, il);
                        nst++;
                        q += 5 + il;
                    }
                    have_pmt = true;
                    for (int i = 0; i < nst; i++)
                        if (st[i].stype == 0x1B || st[i].stype == 0x24 || st[i].stype == 0xEA || st[i].stype == 0x02 || st[i].stype == 0x10) { video_pid = st[i].pid; break; }
                }
            } else if (have_pmt && pid == video_pid) {
                const bool pusi = (p[1] & 0x40) != 0;
                if (pusi) vstarted = true;
                if (!vstarted) continue;
                const int afl = (p[3] >> 4) & 3;
                if (!(afl & 1)) continue;
                int o2 = 4;
                if (afl & 2) o2 += p[4] + 1;
                if (o2 >= TS_PKT) continue;
                const int pl = TS_PKT - o2;
                const int room = (int)sizeof vbuf - vlen;
                memcpy(vbuf + vlen, p + o2, (size_t)(pl < room ? pl : room));
                vlen += pl < room ? pl : room;
            }
        }
        pos += (uint64_t)n - (uint64_t)n % TS_PKT;
    }
    if (!have_pmt) { free(bufs); set_err(err, cap, TR("no program in the first megabytes")); return false; }

    // the picture
    const TsStream *vs = NULL;
    for (int i = 0; i < nst; i++) if (st[i].pid == video_pid) vs = &st[i];
    if (!vs) {
        o->video_ok = false;
        snprintf(o->video_reason, sizeof o->video_reason, "no video");
        snprintf(o->video_desc, sizeof o->video_desc, TR("No video"));
    } else {
        o->video_id = vs->pid;
        const char *codec = "Video";
        H264SpsInfo sps; bool have = false;
        o->video_ok = false;
        switch (vs->stype) {
        case 0x1B: {
            codec = "H.264";
            // the first SPS in the access unit bytes
            for (int i = 0; i + 4 < vlen; i++) {
                if (vbuf[i] == 0 && vbuf[i + 1] == 0 && vbuf[i + 2] == 1 && (vbuf[i + 3] & 0x1F) == 7) {
                    int e = i + 4;
                    while (e + 2 < vlen && !(vbuf[e] == 0 && vbuf[e + 1] == 0 && (vbuf[e + 2] == 1 || vbuf[e + 2] == 0))) e++;
                    if (e + 2 >= vlen) e = vlen;
                    if (h264_parse_sps(vbuf + i + 3, e - (i + 3), &sps)) have = true;
                    break;
                }
            }
            if (have) {
                o->width = sps.width; o->height = sps.height;
                if (sps.has_timing && sps.fps > 0 && sps.fps < 130) o->fps = sps.fps;
                char r2[48] = "";
                o->video_ok = h264_playable(&sps, r2, sizeof r2);
                snprintf(o->video_reason, sizeof o->video_reason, "%s", r2);
            } else {
                o->video_ok = true;                      // nothing to judge it by: let the decoder try
                o->video_reason[0] = '\0';
            }
            break;
        }
        case 0x24: codec = "HEVC";   snprintf(o->video_reason, sizeof o->video_reason, "HEVC"); break;
        case 0xEA: codec = "VC-1";   snprintf(o->video_reason, sizeof o->video_reason, "VC-1"); break;
        case 0x02: codec = "MPEG-2"; snprintf(o->video_reason, sizeof o->video_reason, "MPEG-2"); break;
        default:   snprintf(o->video_reason, sizeof o->video_reason, "unsupported video"); break;
        }
        make_video_desc(o, codec, have ? &sps : NULL);
    }

    free(bufs);                                       // (the first access units were only needed for the picture)

    // audio and subtitle streams
    for (int i = 0; i < nst; i++) {
        const TsStream *s = &st[i];
        LocalAudioCodec codec = LA_OTHER; bool dec = false, ma = false;
        switch (s->stype) {
        case 0x03: case 0x04: codec = LA_MP3; dec = true; break;
        case 0x81: codec = LA_AC3; dec = true; break;
        case 0x82: codec = LA_DTS; dec = true; break;
        case 0x85: codec = LA_DTS_HD; dec = true; break;
        case 0x86: codec = LA_DTS_HD; dec = true; ma = true; break;
        case 0x8A: codec = LA_DTS_HD; dec = true; break;
        case 0x83: codec = LA_TRUEHD; dec = true; break;
        case 0x84: case 0x87: case 0xA1: codec = LA_EAC3; break;
        case 0xA2: codec = LA_DTS_HD; break;             // the secondary stream of a Blu-ray: not played
        case 0x0F: codec = LA_AAC; dec = true; break;     // AAC in ADTS
        case 0x11: codec = LA_AAC; break;                  // in LATM: not read
        case 0x80: codec = LA_PCM; break;
        case 0x06:
            if (s->ac3_desc) { codec = LA_AC3; dec = true; }
            else if (s->eac3_desc) codec = LA_EAC3;
            else if (s->dts_desc) { codec = LA_DTS; dec = true; }
            else continue;
            break;
        case 0x90:                                        // Blu-ray presentation graphics
            if (o->n_subs < LOCAL_MAX_TRACKS) {
                LocalSub *sb = &o->subs[o->n_subs++];
                memset(sb, 0, sizeof *sb);
                sb->id = s->pid; sb->kind = LS_PGS; sb->usable = false;          // (only a Matroska file's subtitles are read)
                snprintf(sb->lang, sizeof sb->lang, "%s", s->lang[0] ? s->lang : "und");
                char lang[32]; lang_name(sb->lang, lang, sizeof lang);
                snprintf(sb->label, sizeof sb->label, "%s", lang);
            }
            continue;
        default: continue;
        }
        if (o->n_audio >= LOCAL_MAX_TRACKS) continue;
        LocalAudio *a = &o->audio[o->n_audio++];
        memset(a, 0, sizeof *a);
        a->id = s->pid; a->codec = codec; a->decodable = dec;
        a->is_default = o->n_audio == 1;                 // a transport stream has no flag: the first is the one the muxer put first
        snprintf(a->lang, sizeof a->lang, "%s", s->lang[0] ? s->lang : "und");
        make_audio_label(a, ma, NULL);
    }

    // how long
    {
        uint8_t *scratch = (uint8_t *)malloc(256 * 1024);
        if (scratch) {
            StreamLocalIndex idx;
            if (stream_local_index(rdr, rctx, ts_size, scratch, 256 * 1024, &idx)) o->duration_secs = idx.duration_secs;
            free(scratch);
        }
    }
    return true;
}

// ---- entry point -------------------------------------------------------------------------------

static bool ends_with(const char *s, const char *ext) {
    const size_t n = strlen(s), e = strlen(ext);
    return n > e && strcasecmp(s + n - e, ext) == 0;
}

bool local_probe(LocalReadAt rd, void *ctx, uint64_t size, const char *name, LocalInfo *out, char *err, int cap) {
    memset(out, 0, sizeof *out);
    out->size = size;
    uint8_t head[4 + 4 * TS_PKT];
    const int n = rd(ctx, 0, head, (int)sizeof head);
    if (n < 4) { set_err(err, cap, n < 0 ? TR("cannot read the file") : TR("the file is empty")); return false; }
    if (head[0] == 0x1A && head[1] == 0x45 && head[2] == 0xDF && head[3] == 0xA3) return probe_mkv(rd, ctx, size, out, err, cap);
    if (n >= 4 + 3 * M2TS_PKT && head[4] == 0x47 && head[4 + M2TS_PKT] == 0x47 && head[4 + 2 * M2TS_PKT] == 0x47 &&
        !(head[0] == 0x47 && head[TS_PKT] == 0x47 && head[2 * TS_PKT] == 0x47))
        return probe_ts(rd, ctx, size, true, out, err, cap);
    if (ends_with(name, ".ts") || ends_with(name, ".m2ts") || ends_with(name, ".mts") || head[0] == 0x47)
        return probe_ts(rd, ctx, size, false, out, err, cap);
    set_err(err, cap, TR("not a video file this player reads"));
    return false;
}

// ---- track choice ---------------------------------------------------------------------------------

static int codec_rank(const LocalAudio *a, bool passthrough) {
    switch (a->codec) {
    case LA_TRUEHD: return passthrough ? 6 : 7;
    case LA_DTS_HD: return passthrough ? 5 : 6;
    case LA_DTS:    return passthrough ? 4 : 5;
    case LA_AC3:    return passthrough ? 9 : 4;
    case LA_AAC:    return 3;
    case LA_MP3:    return 2;
    case LA_MP2:    return 1;
    default:        return 0;
    }
}

int local_pick_audio(const LocalInfo *info, const LocalAudioPrefs *prefs) {
    int best = -1, best_score = -1;
    bool any_non_commentary = false;
    for (int i = 0; i < info->n_audio; i++)
        if (info->audio[i].decodable && !info->audio[i].commentary) any_non_commentary = true;
    for (int i = 0; i < info->n_audio; i++) {
        const LocalAudio *a = &info->audio[i];
        if (!a->decodable) continue;
        if (a->commentary && any_non_commentary) continue;
        const int score = codec_rank(a, prefs && prefs->passthrough) * 2 + (a->is_default ? 1 : 0);
        if (score > best_score) { best_score = score; best = i; }          // strictly greater: the first of equals stays
    }
    return best;
}

bool local_can_play(const LocalInfo *info, const LocalAudioPrefs *prefs, char *why, int why_cap) {
    char m[160] = "";
    bool ok = true;
    if (!info->video_ok) {
        snprintf(m, sizeof m, TR("The picture (%s) is beyond what the PS3 can play."),
                 info->video_reason[0] ? info->video_reason : TR("unsupported"));
        ok = false;
    } else if (info->n_audio == 0) {
        snprintf(m, sizeof m, TR("This file has no sound track."));
        ok = false;
    } else if (local_pick_audio(info, prefs) < 0) {
        snprintf(m, sizeof m, TR("The PS3 cannot decode this file's audio yet (%.80s)."), info->audio[0].label);
        ok = false;
    }
    set_err(why, why_cap, m);
    return ok;
}

int local_pick_sub(const LocalInfo *info, int audio_index) {
    if (audio_index < 0 || audio_index >= info->n_audio) return -1;
    const char *lang = info->audio[audio_index].lang;
    char want[32];
    lang_name(lang, want, sizeof want);
    for (int i = 0; i < info->n_subs; i++) {
        const LocalSub *s = &info->subs[i];
        if (!s->usable || !s->forced) continue;
        char have[32];
        lang_name(s->lang, have, sizeof have);
        if (strcmp(have, want) == 0) return i;
    }
    return -1;
}
