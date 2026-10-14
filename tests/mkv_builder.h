#pragma once
// A small Matroska writer and an in-memory reader for the host tests (test_mkv_demux.cpp,
// test_mkv_ts.cpp): enough EBML to build the files muxers rarely produce (lacing in every mode,
// unknown sizes, header stripping, odd NAL lengths) with known contents.

#include "mkv_demux.h"

#include <algorithm>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#pragma GCC diagnostic ignored "-Wunused-function"

// ---- a small EBML writer ----------------------------------------------------------------------

typedef std::string Bytes;

static Bytes id_bytes(uint32_t id) {
    Bytes b;
    int n = id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 0xFF ? 2 : 1;
    for (int i = n - 1; i >= 0; i--) b.push_back((char)(id >> (8 * i)));
    return b;
}
static Bytes size_bytes(uint64_t size, int min_len = 1) {
    int n = min_len;
    while (n < 8 && size >= ((1ull << (7 * n)) - 1)) n++;
    Bytes b((size_t)n, '\0');
    uint64_t v = size;
    for (int i = n - 1; i >= 0; i--) { b[(size_t)i] = (char)(v & 0xFF); v >>= 8; }
    b[0] = (char)(b[0] | (0x80 >> (n - 1)));
    return b;
}
static Bytes unknown_size() { return Bytes("\x01\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8); }
static Bytes elem(uint32_t id, const Bytes &payload) { return id_bytes(id) + size_bytes(payload.size()) + payload; }
static Bytes uint_el(uint32_t id, uint64_t v) {
    Bytes p;
    int n = 1;
    while (n < 8 && (v >> (8 * n))) n++;
    for (int i = n - 1; i >= 0; i--) p.push_back((char)(v >> (8 * i)));
    return elem(id, p);
}
static Bytes float_el(uint32_t id, double d) {
    uint64_t u; memcpy(&u, &d, 8);
    Bytes p;
    for (int i = 7; i >= 0; i--) p.push_back((char)(u >> (8 * i)));
    return elem(id, p);
}
static Bytes str_el(uint32_t id, const std::string &s) { return elem(id, s); }

enum { ID_EBML = 0x1A45DFA3, ID_DOCTYPE = 0x4282, ID_SEGMENT = 0x18538067, ID_SEEKHEAD = 0x114D9B74, ID_SEEK = 0x4DBB,
       ID_SEEKID = 0x53AB, ID_SEEKPOS = 0x53AC, ID_INFO = 0x1549A966, ID_TIMESCALE = 0x2AD7B1, ID_DURATION = 0x4489,
       ID_TRACKS = 0x1654AE6B, ID_TRACKENTRY = 0xAE, ID_TRACKNUMBER = 0xD7, ID_TRACKTYPE = 0x83, ID_CODECID = 0x86,
       ID_CODECPRIVATE = 0x63A2, ID_LANGUAGE = 0x22B59C, ID_NAME = 0x536E, ID_FLAGDEFAULT = 0x88, ID_FLAGFORCED = 0x55AA,
       ID_DEFAULTDURATION = 0x23E383, ID_VIDEO = 0xE0, ID_PIXELW = 0xB0, ID_PIXELH = 0xBA, ID_AUDIO = 0xE1,
       ID_SAMPLERATE = 0xB5, ID_CHANNELS = 0x9F, ID_CONTENTENCODINGS = 0x6D80, ID_CONTENTENCODING = 0x6240,
       ID_ENCTYPE = 0x5033, ID_ENCCOMP = 0x5034, ID_COMPALGO = 0x4254, ID_COMPSETTINGS = 0x4255, ID_ENCENC = 0x5035,
       ID_CUES = 0x1C53BB6B, ID_CUEPOINT = 0xBB, ID_CUETIME = 0xB3, ID_CUETRACKPOS = 0xB7, ID_CUETRACK = 0xF7,
       ID_CUECLUSTERPOS = 0xF1, ID_CLUSTER = 0x1F43B675, ID_TIMECODE = 0xE7, ID_SIMPLEBLOCK = 0xA3,
       ID_BLOCKGROUP = 0xA0, ID_BLOCK = 0xA1, ID_BLOCKDURATION = 0x9B, ID_REFERENCEBLOCK = 0xFB, ID_VOID = 0xEC,
       ID_TAGS = 0x1254C367 };

static Bytes vint(uint64_t v, int len) {
    Bytes b((size_t)len, '\0');
    for (int i = len - 1; i >= 0; i--) { b[(size_t)i] = (char)(v & 0xFF); v >>= 8; }
    b[0] = (char)(b[0] | (0x80 >> (len - 1)));
    return b;
}

// A Block's payload: track vint, 16-bit relative time, flags, [lacing header], frames.
static Bytes block_payload(int track, int rel, uint8_t flags, const std::vector<Bytes> &frames, int lacing) {
    Bytes b = vint((uint64_t)track, 1);
    b.push_back((char)((rel >> 8) & 0xFF)); b.push_back((char)(rel & 0xFF));
    b.push_back((char)(flags | (lacing << 1)));
    if (lacing) {
        b.push_back((char)(frames.size() - 1));
        if (lacing == 1) {                                  // Xiph
            for (size_t i = 0; i + 1 < frames.size(); i++) {
                size_t s = frames[i].size();
                while (s >= 255) { b.push_back((char)255); s -= 255; }
                b.push_back((char)s);
            }
        } else if (lacing == 3 && frames.size() >= 2) {     // EBML
            b += vint(frames[0].size(), 2);
            int64_t prev = (int64_t)frames[0].size();
            for (size_t i = 1; i + 1 < frames.size(); i++) {
                const int64_t d = (int64_t)frames[i].size() - prev;
                b += vint((uint64_t)(d + 8191), 2);          // 2-byte bias is 2^13 - 1
                prev = (int64_t)frames[i].size();
            }
        }                                                   // fixed: no sizes
    }
    for (auto &f : frames) b += f;
    return b;
}
static Bytes simple_block(int track, int rel, bool key, const std::vector<Bytes> &frames, int lacing = 0) {
    return elem(ID_SIMPLEBLOCK, block_payload(track, rel, key ? 0x80 : 0, frames, lacing));
}

static Bytes frame_of(int seed, size_t n) {
    Bytes b(n, '\0');
    for (size_t i = 0; i < n; i++) b[i] = (char)((seed * 31 + (int)i * 7) & 0xFF);
    return b;
}

static Bytes avcc(int nal_len = 4) {
    static const uint8_t sps[] = { 0x67, 0x64, 0x00, 0x28, 0xAC, 0xD9, 0x40, 0x78, 0x02, 0x27, 0xE5, 0x84, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xF0, 0x3C, 0x60, 0xC6, 0x58 };
    static const uint8_t pps[] = { 0x68, 0xEB, 0xE3, 0xCB, 0x22, 0xC0 };
    Bytes b;
    b.push_back(1); b.push_back(0x64); b.push_back(0); b.push_back(0x28); b.push_back((char)(0xFC | (nal_len - 1)));
    b.push_back((char)0xE1);
    b.push_back(0); b.push_back((char)sizeof sps); b.append((const char *)sps, sizeof sps);
    b.push_back(1);
    b.push_back(0); b.push_back((char)sizeof pps); b.append((const char *)pps, sizeof pps);
    return b;
}

static Bytes track_video(int num, const Bytes &cp) {
    return elem(ID_TRACKENTRY, uint_el(ID_TRACKNUMBER, (uint64_t)num) + uint_el(ID_TRACKTYPE, 1) +
        str_el(ID_CODECID, "V_MPEG4/ISO/AVC") + elem(ID_CODECPRIVATE, cp) + uint_el(ID_DEFAULTDURATION, 41708333) +
        elem(ID_VIDEO, uint_el(ID_PIXELW, 1280) + uint_el(ID_PIXELH, 720)));
}
static Bytes track_audio(int num, const char *codec, const char *lang, int ch, bool def = true, uint64_t dd = 0) {
    Bytes t = uint_el(ID_TRACKNUMBER, (uint64_t)num) + uint_el(ID_TRACKTYPE, 2) + str_el(ID_CODECID, codec) +
        str_el(ID_LANGUAGE, lang) + uint_el(ID_FLAGDEFAULT, def ? 1 : 0) +
        elem(ID_AUDIO, float_el(ID_SAMPLERATE, 48000.0) + uint_el(ID_CHANNELS, (uint64_t)ch));
    if (dd) t += uint_el(ID_DEFAULTDURATION, dd);
    return elem(ID_TRACKENTRY, t);
}
static Bytes track_sub(int num, const char *codec, const char *name, bool forced) {
    return elem(ID_TRACKENTRY, uint_el(ID_TRACKNUMBER, (uint64_t)num) + uint_el(ID_TRACKTYPE, 17) +
        str_el(ID_CODECID, codec) + str_el(ID_NAME, name) + uint_el(ID_FLAGFORCED, forced ? 1 : 0) + str_el(ID_LANGUAGE, "eng"));
}

struct Built {
    Bytes file;
    uint64_t seg_data;                 // where the Segment's data starts
    std::vector<uint64_t> cluster_pos; // each cluster, from the segment data start
};

struct Opts {
    bool unknown_segment = false, unknown_clusters = false, cues = true, cues_at_end = true, seekhead_cues = true;
};

static Built build(const Bytes &tracks, const std::vector<Bytes> &clusters_payload, const std::vector<uint64_t> &cluster_ticks,
                   const Opts &o, double duration_ticks = 10000.0) {
    Built out;
    Bytes head = elem(ID_EBML, str_el(ID_DOCTYPE, "matroska"));
    Bytes info = elem(ID_INFO, uint_el(ID_TIMESCALE, 1000000) + float_el(ID_DURATION, duration_ticks));
    Bytes trk = elem(ID_TRACKS, tracks);

    // clusters, with their positions relative to the segment data
    std::vector<Bytes> clusters;
    for (size_t i = 0; i < clusters_payload.size(); i++) {
        Bytes body = uint_el(ID_TIMECODE, cluster_ticks[i]) + clusters_payload[i];
        clusters.push_back(o.unknown_clusters ? id_bytes(ID_CLUSTER) + unknown_size() + body : elem(ID_CLUSTER, body));
    }
    // the SeekHead has a fixed size, so the layout can be computed in one pass
    auto seekhead = [&](uint64_t cues_pos) {
        Bytes pos;                                      // SeekPosition: a plain 4-byte unsigned integer
        for (int i = 3; i >= 0; i--) pos.push_back((char)(cues_pos >> (8 * i)));
        return elem(ID_SEEKHEAD, elem(ID_SEEK, elem(ID_SEEKID, id_bytes(ID_CUES)) + elem(ID_SEEKPOS, pos)));
    };
    const size_t sh_len = o.seekhead_cues && o.cues ? seekhead(0).size() : 0;
    uint64_t pos = sh_len + info.size() + trk.size();
    for (auto &c : clusters) { out.cluster_pos.push_back(pos); pos += c.size(); }
    Bytes cues;
    if (o.cues) {
        Bytes pts;
        for (size_t i = 0; i < clusters.size(); i++)
            pts += elem(ID_CUEPOINT, uint_el(ID_CUETIME, cluster_ticks[i]) +
                        elem(ID_CUETRACKPOS, uint_el(ID_CUETRACK, 1) + uint_el(ID_CUECLUSTERPOS, out.cluster_pos[i])));
        cues = elem(ID_CUES, pts);
    }
    Bytes seg;
    if (o.cues && o.seekhead_cues) seg += seekhead(pos);
    seg += info + trk;
    for (auto &c : clusters) seg += c;
    if (o.cues) seg += cues;
    out.file = head + id_bytes(ID_SEGMENT) + (o.unknown_segment ? unknown_size() : size_bytes(seg.size(), 8)) + seg;
    out.seg_data = head.size() + 4 + 8;                // the Segment's 4-byte id and 8-byte size
    return out;
}

// ---- reading from memory -------------------------------------------------------------------------

struct Mem { const Bytes *b; int fail_after; int reads; };
static int mem_read(void *ctx, uint64_t off, uint8_t *buf, int len) {
    Mem *m = (Mem *)ctx;
    m->reads++;
    if (m->fail_after >= 0 && m->reads > m->fail_after) return -1;
    if (off >= m->b->size()) return 0;
    const size_t n = std::min<size_t>((size_t)len, m->b->size() - (size_t)off);
    memcpy(buf, m->b->data() + off, n);
    return (int)n;
}

struct Fr { int track; int64_t pts; bool key; Bytes data; uint64_t dur; };
static std::vector<Fr> read_all(MkvFile *f, uint32_t cap = 1 << 20, MkvReader *keep = nullptr) {
    static std::vector<uint8_t> buf;
    buf.assign(cap, 0);
    MkvReader r;
    mkv_reader_init(&r, f, buf.data(), cap);
    std::vector<Fr> out;
    MkvFrame fr;
    int rc;
    while ((rc = mkv_next_frame(&r, &fr)) == 1) {
        Bytes d((const char *)fr.data, fr.size);
        if (fr.prefix_len) d = Bytes((const char *)fr.prefix, (size_t)fr.prefix_len) + d;
        out.push_back({ fr.track, fr.pts_ns, fr.key, d, fr.duration_ns });
    }
    if (keep) *keep = r;
    return out;
}

