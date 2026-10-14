// Host test for source/local/local_tags.c: what a music file says about itself.
//
// The fixtures (tests/fixtures/music, made by make.sh with ffmpeg) are real FLAC and MP3 files; the
// rest of the cases are built here byte by byte, so every tag version, text encoding and flag the
// reader handles has a file that uses it.  Damaged files are the other half: every fixture is cut
// short at many lengths and has bytes flipped, and the reader must answer without reading outside
// the file (the sanitizers build catches an overread) and with fields that stay inside their bounds.
//
//   make -f Makefile.host test_local_tags && ./test_local_tags

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "local_tags.h"

typedef std::vector<uint8_t> Bytes;

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---------------------------------------------------------------------------
//  A file in memory, read the way lfs_read reads one
// ---------------------------------------------------------------------------

struct Mem {
    const Bytes *b;
    int max_chunk;          // a read returns at most this many bytes (0 = as asked)
    bool overread;          // a read asked for bytes outside the file
};

static int mem_read(void *c, uint64_t off, uint8_t *buf, int len) {
    Mem *m = (Mem *)c;
    if (len < 0 || off > m->b->size() || (uint64_t)len > m->b->size() - off) m->overread = true;
    if (off >= m->b->size()) return 0;
    uint64_t n = m->b->size() - off;
    if (n > (uint64_t)len) n = (uint64_t)len;
    if (m->max_chunk > 0 && n > (uint64_t)m->max_chunk) n = (uint64_t)m->max_chunk;
    memcpy(buf, m->b->data() + off, (size_t)n);
    return (int)n;
}

static bool load(const char *name, Bytes *out) {
    std::string p = std::string("fixtures/music/") + name;
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) { printf("cannot open %s (run from tests/)\n", p.c_str()); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out->resize((size_t)n);
    const size_t got = fread(out->data(), 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

static bool meta_of(const Bytes &b, LaKind kind, LaMeta *m, int chunk = 0) {
    Mem mem = { &b, chunk, false };
    const bool ok = la_read_meta(mem_read, &mem, b.size(), kind, m);
    CHECK(!mem.overread);
    return ok;
}

// ---------------------------------------------------------------------------
//  Builders
// ---------------------------------------------------------------------------

static void le16(Bytes &b, uint32_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
static void le32(Bytes &b, uint32_t v) { le16(b, v & 0xFFFF); le16(b, v >> 16); }
static void be16(Bytes &b, uint32_t v) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
static void be24(Bytes &b, uint32_t v) { b.push_back((uint8_t)(v >> 16)); be16(b, v & 0xFFFF); }
static void be32(Bytes &b, uint32_t v) { be16(b, v >> 16); be16(b, v & 0xFFFF); }
static void put(Bytes &b, const char *s) { while (*s) b.push_back((uint8_t)*s++); }
static void put(Bytes &b, const Bytes &o) { b.insert(b.end(), o.begin(), o.end()); }
static Bytes bytes_of(const char *s) { Bytes b; put(b, s); return b; }

// ---- FLAC ----
static Bytes streaminfo(int rate, int ch, int bps, uint64_t total) {
    Bytes b;
    be16(b, 4096); be16(b, 4096); be24(b, 0); be24(b, 0);
    const uint64_t v = ((uint64_t)rate << 44) | ((uint64_t)(ch - 1) << 41) | ((uint64_t)(bps - 1) << 36) | total;
    for (int i = 7; i >= 0; i--) b.push_back((uint8_t)(v >> (8 * i)));
    for (int i = 0; i < 16; i++) b.push_back(0);
    return b;
}
static Bytes block(int type, bool last, const Bytes &body) {
    Bytes b;
    b.push_back((uint8_t)((last ? 0x80 : 0) | type));
    be24(b, (uint32_t)body.size());
    put(b, body);
    return b;
}
static Bytes vorbis(const std::vector<std::string> &fields) {
    Bytes b;
    const char *vendor = "test";
    le32(b, 4); put(b, vendor);
    le32(b, (uint32_t)fields.size());
    for (const std::string &f : fields) { le32(b, (uint32_t)f.size()); put(b, f.c_str()); }
    return b;
}
static Bytes picture(uint32_t kind, const char *mime, const char *desc, const Bytes &data) {
    Bytes b;
    be32(b, kind);
    be32(b, (uint32_t)strlen(mime)); put(b, mime);
    be32(b, (uint32_t)strlen(desc)); put(b, desc);
    be32(b, 1); be32(b, 1); be32(b, 24); be32(b, 0);
    be32(b, (uint32_t)data.size()); put(b, data);
    return b;
}
static Bytes filler(size_t n, uint8_t v) { return Bytes(n, v); }

// ---- ID3 / MP3 ----
static Bytes syncsafe32(uint32_t v) {
    Bytes b;
    b.push_back((uint8_t)((v >> 21) & 0x7F)); b.push_back((uint8_t)((v >> 14) & 0x7F));
    b.push_back((uint8_t)((v >> 7) & 0x7F));  b.push_back((uint8_t)(v & 0x7F));
    return b;
}
static Bytes id3_tag(int ver, int flags, const Bytes &body) {
    Bytes b;
    put(b, "ID3");
    b.push_back((uint8_t)ver); b.push_back(0); b.push_back((uint8_t)flags);
    put(b, syncsafe32((uint32_t)body.size()));
    put(b, body);
    return b;
}
static Bytes frame23(const char *id, const Bytes &body, int flags2 = 0) {
    Bytes b;
    put(b, id); be32(b, (uint32_t)body.size()); b.push_back(0); b.push_back((uint8_t)flags2);
    put(b, body);
    return b;
}
static Bytes frame24(const char *id, const Bytes &body, int flags2 = 0) {
    Bytes b;
    put(b, id); put(b, syncsafe32((uint32_t)body.size())); b.push_back(0); b.push_back((uint8_t)flags2);
    put(b, body);
    return b;
}
static Bytes frame22(const char *id, const Bytes &body) {
    Bytes b;
    put(b, id); be24(b, (uint32_t)body.size());
    put(b, body);
    return b;
}
static Bytes text_body(int enc, const Bytes &text) {
    Bytes b;
    b.push_back((uint8_t)enc);
    put(b, text);
    return b;
}
static Bytes latin1(const char *s) { return bytes_of(s); }
static Bytes utf16(const char *ascii, bool big, bool bom) {
    Bytes b;
    if (bom) { if (big) { b.push_back(0xFE); b.push_back(0xFF); } else { b.push_back(0xFF); b.push_back(0xFE); } }
    for (const char *p = ascii; *p; p++) {
        if (big) { b.push_back(0); b.push_back((uint8_t)*p); } else { b.push_back((uint8_t)*p); b.push_back(0); }
    }
    return b;
}

// An MPEG 1 Layer III frame at 44.1 kHz, 128 kbit/s, stereo: 417 bytes.
static Bytes mp3_frames(int n, bool crc_off = true) {
    Bytes b;
    for (int i = 0; i < n; i++) {
        b.push_back(0xFF); b.push_back(crc_off ? 0xFB : 0xFA); b.push_back(0x90); b.push_back(0x00);
        for (int j = 4; j < 417; j++) b.push_back(0x11);
    }
    return b;
}

// ---- WAVE ----
static Bytes riff(const Bytes &chunks) {
    Bytes b;
    put(b, "RIFF"); le32(b, (uint32_t)(4 + chunks.size())); put(b, "WAVE"); put(b, chunks);
    return b;
}
static Bytes chunk(const char *id, const Bytes &body) {
    Bytes b;
    put(b, id); le32(b, (uint32_t)body.size()); put(b, body);
    if (body.size() & 1) b.push_back(0);
    return b;
}
static Bytes wav_fmt(int format, int ch, int rate, int bits) {
    Bytes b;
    le16(b, (uint32_t)format); le16(b, (uint32_t)ch); le32(b, (uint32_t)rate);
    le32(b, (uint32_t)(rate * ch * (bits / 8))); le16(b, (uint32_t)(ch * (bits / 8))); le16(b, (uint32_t)bits);
    return b;
}
static Bytes wav_file(int format, int ch, int rate, int bits, int frames, const Bytes &extra_before_data = Bytes()) {
    Bytes c;
    put(c, chunk("fmt ", wav_fmt(format, ch, rate, bits)));
    put(c, extra_before_data);
    put(c, chunk("data", filler((size_t)frames * (size_t)ch * (size_t)(bits / 8), 0x20)));
    return riff(c);
}

// ---------------------------------------------------------------------------
//  Tests
// ---------------------------------------------------------------------------

static void test_flac_fixtures() {
    printf("- FLAC fixtures\n");
    Bytes b;
    LaMeta m;
    if (!load("chirp44.flac", &b)) { CHECK(false); return; }
    CHECK(meta_of(b, LAF_FLAC, &m));
    CHECK(m.kind == LAF_FLAC && m.sample_rate == 44100 && m.channels == 2 && m.bits == 16);
    CHECK(m.total_frames == 88200 && m.duration_secs == 2);
    CHECK(!strcmp(m.title, "Chirp \xE2\x98\x83 Test"));                    // a snowman: UTF-8 through
    CHECK(!strcmp(m.artist, "A. Tester") && !strcmp(m.album, "Fixtures"));
    CHECK(m.track_no == 3 && m.disc_no == 1);
    CHECK(m.pic_len > 0 && !strcmp(m.pic_mime, "image/jpeg") && m.pic_off + m.pic_len <= b.size());
    CHECK(m.pic_len > 4 && b[m.pic_off] == 0xFF && b[m.pic_off + 1] == 0xD8 && b[m.pic_off + 2] == 0xFF);
    CHECK(m.stream_off == 0 && m.data_off > 4 && m.data_off + m.data_len == b.size());
    CHECK(b[m.data_off] == 0xFF && (b[m.data_off + 1] & 0xFE) == 0xF8);        // the first frame's sync code
    CHECK(m.flac_max_block > 0);
    // reads that come back in small pieces change nothing
    LaMeta m2;
    CHECK(meta_of(b, LAF_FLAC, &m2, 7));
    CHECK(!memcmp(&m, &m2, sizeof m));

    CHECK(load("chirp96_24.flac", &b) && meta_of(b, LAF_FLAC, &m));
    CHECK(m.sample_rate == 96000 && m.bits == 24 && m.channels == 2 && m.duration_secs == 1 && m.title[0] == '\0' && m.pic_len == 0);
    CHECK(load("surround6.flac", &b) && meta_of(b, LAF_FLAC, &m));
    CHECK(m.channels == 6 && m.sample_rate == 48000 && m.total_frames == 24000);
    CHECK(load("chirp48.flac", &b) && meta_of(b, LAF_FLAC, &m));
    CHECK(m.sample_rate == 48000 && m.bits == 16 && m.total_frames == 96000);
}

static void test_flac_synthetic() {
    printf("- FLAC blocks\n");
    LaMeta m;
    const Bytes tail = filler(64, 0xAA);                       // stands in for the audio
    auto file = [&](const std::vector<Bytes> &blocks) {
        Bytes b = bytes_of("fLaC");
        put(b, block(0, blocks.empty(), streaminfo(48000, 2, 16, 48000 * 3)));
        for (size_t i = 0; i < blocks.size(); i++) put(b, blocks[i]);
        put(b, tail);
        return b;
    };
    // the last-block flag belongs to the last block written
    auto with = [&](int type, const Bytes &body) { return block(type, false, body); };

    {   // no STREAMINFO is not a FLAC file
        Bytes b = bytes_of("fLaC");
        put(b, block(4, true, vorbis({ "TITLE=x" })));
        put(b, tail);
        CHECK(!meta_of(b, LAF_FLAC, &m));
    }
    {   // ARTIST wins over ALBUMARTIST whatever the order; ALBUMARTIST stands in alone; keys in any case; TRACKNUMBER "05/12"
        Bytes b = bytes_of("fLaC");
        put(b, with(0, streaminfo(48000, 2, 16, 48000 * 3)));
        put(b, block(4, true, vorbis({ "albumartist=Group", "Title=Song", "ARTIST=Solo", "TrackNumber=05/12", "DISCNUMBER=2", "ALBUM=LP" })));
        put(b, tail);
        CHECK(meta_of(b, LAF_FLAC, &m));
        CHECK(!strcmp(m.artist, "Solo") && !strcmp(m.title, "Song") && !strcmp(m.album, "LP") && m.track_no == 5 && m.disc_no == 2);
        CHECK(m.duration_secs == 3 && m.data_off == b.size() - tail.size());
    }
    {
        Bytes b = bytes_of("fLaC");
        put(b, with(0, streaminfo(48000, 2, 16, 0)));
        put(b, block(4, true, vorbis({ "ALBUMARTIST=Group" })));
        CHECK(meta_of(b, LAF_FLAC, &m) && !strcmp(m.artist, "Group") && m.duration_secs == 0 && m.total_frames == 0);
    }
    {   // control characters and bad UTF-8 are cleaned: a lone 0xC3 and a newline
        Bytes b = bytes_of("fLaC");
        put(b, with(0, streaminfo(48000, 2, 16, 48000)));
        Bytes v = vorbis({ "TITLE=a\nb" });
        Bytes bad = vorbis({ "TITLE=xy" });
        bad[bad.size() - 1] = 0xC3;
        put(b, with(4, v));
        put(b, block(4, true, bad));
        CHECK(meta_of(b, LAF_FLAC, &m) && !strcmp(m.title, "x\xEF\xBF\xBD"));
        Bytes b2 = bytes_of("fLaC");
        put(b2, with(0, streaminfo(48000, 2, 16, 48000)));
        put(b2, block(4, true, v));
        CHECK(meta_of(b2, LAF_FLAC, &m) && !strcmp(m.title, "a b"));
    }
    {   // the front cover wins over an earlier picture of another kind; a second front cover does not replace the first
        Bytes other = filler(40, 0x11), front = filler(50, 0x22), front2 = filler(60, 0x33);
        Bytes b = bytes_of("fLaC");
        put(b, with(0, streaminfo(48000, 2, 16, 1000)));
        put(b, with(6, picture(0, "image/png", "", other)));
        put(b, with(6, picture(3, "image/jpeg", "the cover", front)));
        put(b, block(6, true, picture(3, "image/jpeg", "", front2)));
        put(b, tail);
        CHECK(meta_of(b, LAF_FLAC, &m));
        CHECK(m.pic_len == 50 && !strcmp(m.pic_mime, "image/jpeg") && b[m.pic_off] == 0x22 && b[m.pic_off + 49] == 0x22);
        // alone, any picture will do
        Bytes c = bytes_of("fLaC");
        put(c, with(0, streaminfo(48000, 2, 16, 1000)));
        put(c, block(6, true, picture(4, "image/png", "back", other)));
        CHECK(meta_of(c, LAF_FLAC, &m) && m.pic_len == 40 && !strcmp(m.pic_mime, "image/png") && c[m.pic_off] == 0x11);
        // a picture that is a link ("-->") is not bytes
        Bytes d = bytes_of("fLaC");
        put(d, with(0, streaminfo(48000, 2, 16, 1000)));
        put(d, block(6, true, picture(3, "-->", "", bytes_of("http://x"))));
        CHECK(meta_of(d, LAF_FLAC, &m) && m.pic_len == 0);
        // a picture whose length runs past its block is dropped
        Bytes e = bytes_of("fLaC");
        put(e, with(0, streaminfo(48000, 2, 16, 1000)));
        Bytes p = picture(3, "image/png", "", other);
        p[p.size() - 40 - 1] = 0x7F;                                  // the data length's low byte
        put(e, block(6, true, p));
        CHECK(meta_of(e, LAF_FLAC, &m) && m.pic_len == 0);
    }
    {   // a big block is skipped without being read: a megabyte of padding between the blocks
        Bytes b = bytes_of("fLaC");
        put(b, with(0, streaminfo(48000, 2, 16, 48000)));
        put(b, with(1, filler(1 << 20, 0)));
        put(b, block(4, true, vorbis({ "TITLE=After padding" })));
        put(b, tail);
        CHECK(meta_of(b, LAF_FLAC, &m) && !strcmp(m.title, "After padding") && m.data_off == b.size() - tail.size());
    }
    {   // an ID3v2 tag in front of the stream
        Bytes inner = file({});
        Bytes b = id3_tag(3, 0, filler(100, 0));
        const size_t pre = b.size();
        put(b, inner);
        CHECK(meta_of(b, LAF_FLAC, &m) && m.stream_off == pre && m.data_off == pre + inner.size() - tail.size() && m.sample_rate == 48000);
    }
    {   // a block that claims more than the file holds
        Bytes b = bytes_of("fLaC");
        put(b, with(0, streaminfo(48000, 2, 16, 48000)));
        Bytes big = block(4, true, vorbis({ "TITLE=x" }));
        big[1] = 0x7F;
        put(b, big);
        CHECK(!meta_of(b, LAF_FLAC, &m));
    }
    CHECK(!meta_of(bytes_of("not a flac file at all, no"), LAF_FLAC, &m));
}

static void test_mp3_fixtures() {
    printf("- MP3 fixtures\n");
    Bytes b;
    LaMeta m;
    CHECK(load("chirp44_v24.mp3", &b) && meta_of(b, LAF_MP3, &m));
    CHECK(m.kind == LAF_MP3 && m.sample_rate == 44100 && m.channels == 2 && m.mp3_spf == 1152);
    CHECK(!strcmp(m.title, "Chirp \xE2\x98\x83 Test") && !strcmp(m.artist, "A. Tester") && !strcmp(m.album, "Fixtures"));
    CHECK(m.track_no == 3 && m.disc_no == 1);
    CHECK(m.pic_len > 0 && !strcmp(m.pic_mime, "image/jpeg") && m.pic_off + m.pic_len <= b.size());
    CHECK(b[m.pic_off] == 0xFF && b[m.pic_off + 1] == 0xD8);
    CHECK(m.mp3_gapless && m.mp3_skip == 1105 && m.total_frames == 88200 && m.duration_secs == 2);
    CHECK(m.mp3_has_toc && m.bitrate_kbps >= 120 && m.bitrate_kbps <= 140);
    CHECK(b[m.data_off] == 0xFF && (b[m.data_off + 1] & 0xE0) == 0xE0 && m.data_off > m.pic_off + m.pic_len);
    CHECK(m.data_off + m.data_len == b.size());
    LaMeta m2;
    CHECK(meta_of(b, LAF_MP3, &m2, 11) && !memcmp(&m, &m2, sizeof m));

    CHECK(load("chirp44_v23.mp3", &b) && meta_of(b, LAF_MP3, &m));
    CHECK(!strcmp(m.title, "Chirp v2.3") && !strcmp(m.artist, "B. Tester") && !strcmp(m.album, "Old Tags") && m.track_no == 7);
    CHECK(m.pic_len > 0 && b[m.pic_off] == 0xFF && b[m.pic_off + 1] == 0xD8);

    CHECK(load("plain_v1.mp3", &b) && meta_of(b, LAF_MP3, &m));
    CHECK(!strcmp(m.title, "Plain") && !strcmp(m.artist, "C. Tester") && !strcmp(m.album, "Tiny") && m.track_no == 5);
    CHECK(!m.mp3_gapless && m.mp3_skip == 0 && !m.mp3_has_toc && m.pic_len == 0);
    CHECK(m.data_off == 0 || m.data_off < 200);                                  // no ID3v2: the stream starts at once (after the Info frame ffmpeg may leave out)
    CHECK(m.data_off + m.data_len == b.size() - 128);                           // the ID3v1 tag is not audio
    CHECK(m.bitrate_kbps == 128 && m.duration_secs >= 1 && m.duration_secs <= 2);

    CHECK(load("mono22.mp3", &b) && meta_of(b, LAF_MP3, &m));
    CHECK(m.sample_rate == 22050 && m.channels == 1 && m.mp3_spf == 576 && m.duration_secs == 1);
}

static void test_mp3_synthetic() {
    printf("- MP3 tags\n");
    LaMeta m;
    auto assemble = [&](const Bytes &tag, const Bytes &audio) { Bytes b = tag; put(b, audio); return b; };

    {   // v2.2: three-letter frame ids, PIC with a three-letter image format
        Bytes pic = text_body(0, Bytes());
        put(pic, "PNG"); pic.push_back(3); pic.push_back(0);               // type 3, empty description
        const Bytes data = filler(80, 0x5A);
        put(pic, data);
        Bytes body;
        put(body, frame22("TT2", text_body(0, latin1("Old Title"))));
        put(body, frame22("TP1", text_body(0, latin1("Old Artist"))));
        put(body, frame22("TAL", text_body(0, latin1("Old Album"))));
        put(body, frame22("TRK", text_body(0, latin1("12/20"))));
        put(body, frame22("PIC", pic));
        put(body, filler(30, 0));
        const Bytes tag = id3_tag(2, 0, body);
        const Bytes b = assemble(tag, mp3_frames(3));
        CHECK(meta_of(b, LAF_MP3, &m));
        CHECK(!strcmp(m.title, "Old Title") && !strcmp(m.artist, "Old Artist") && !strcmp(m.album, "Old Album") && m.track_no == 12);
        CHECK(m.pic_len == 80 && !strcmp(m.pic_mime, "image/png") && b[m.pic_off] == 0x5A && b[m.pic_off - 1] == 0);
        CHECK(m.data_off == tag.size());
    }
    {   // v2.3: UTF-16 with a byte order mark (both orders), Latin-1 with an accent, front cover chosen over another
        Bytes apic_other = text_body(0, latin1("image/gif")); apic_other.push_back(0); apic_other.push_back(0); apic_other.push_back(0); put(apic_other, filler(20, 0x10));
        Bytes apic_front = text_body(0, latin1("image/jpg")); apic_front.push_back(0); apic_front.push_back(3); apic_front.push_back(0); put(apic_front, filler(30, 0x20));
        Bytes body;
        put(body, frame23("APIC", apic_other));
        put(body, frame23("TIT2", text_body(1, utf16("Wide", false, true))));
        put(body, frame23("TPE1", text_body(1, utf16("Big End", true, true))));
        put(body, frame23("TALB", text_body(0, latin1("Caf\xE9"))));
        put(body, frame23("TRCK", text_body(0, latin1("4/9"))));
        put(body, frame23("TPOS", text_body(0, latin1("2"))));
        put(body, frame23("APIC", apic_front));
        put(body, frame23("COMM", text_body(0, latin1("ignored"))));
        const Bytes b = assemble(id3_tag(3, 0, body), mp3_frames(3));
        CHECK(meta_of(b, LAF_MP3, &m));
        CHECK(!strcmp(m.title, "Wide") && !strcmp(m.artist, "Big End") && !strcmp(m.album, "Caf\xC3\xA9") && m.track_no == 4 && m.disc_no == 2);
        CHECK(m.pic_len == 30 && !strcmp(m.pic_mime, "image/jpeg") && b[m.pic_off] == 0x20);
    }
    {   // v2.3 with the whole-tag unsynchronisation flag: FF 00 stands for FF; a picture in such a tag cannot be pointed at
        Bytes body;
        put(body, frame23("TIT2", text_body(0, latin1("\xFF" "x"))));          // "ÿx"
        Bytes apic = text_body(0, latin1("image/png")); apic.push_back(0); apic.push_back(3); apic.push_back(0); put(apic, filler(20, 0x77));
        put(body, frame23("APIC", apic));
        Bytes on_disk;
        for (size_t i = 0; i < body.size(); i++) { on_disk.push_back(body[i]); if (body[i] == 0xFF) on_disk.push_back(0x00); }
        const Bytes b = assemble(id3_tag(3, 0x80, on_disk), mp3_frames(3));
        CHECK(meta_of(b, LAF_MP3, &m));
        CHECK(!strcmp(m.title, "\xC3\xBF" "x") && m.pic_len == 0);
    }
    {   // v2.4: syncsafe sizes, UTF-8, several values (the first is used), an extended header, padding, grouping and data length bytes
        Bytes multi = text_body(3, bytes_of("First"));
        multi.push_back(0); put(multi, "Second");
        Bytes grouped; grouped.push_back(0x42); put(grouped, text_body(3, bytes_of("Grouped Artist")));
        Bytes dli; be32(dli, 0); put(dli, text_body(3, bytes_of("With Length")));
        Bytes body;
        put(body, syncsafe32(10)); put(body, filler(6, 0));                      // extended header: 10 bytes
        put(body, frame24("TIT2", multi));
        put(body, frame24("TPE1", grouped, 0x40));
        put(body, frame24("TALB", dli, 0x01));
        put(body, frame24("TRCK", text_body(3, bytes_of("6"))));
        put(body, filler(50, 0));
        const Bytes b = assemble(id3_tag(4, 0x40, body), mp3_frames(3));
        CHECK(meta_of(b, LAF_MP3, &m));
        CHECK(!strcmp(m.title, "First") && !strcmp(m.artist, "Grouped Artist") && !strcmp(m.album, "With Length") && m.track_no == 6);
    }
    {   // v2.4: a frame that is compressed or encrypted is left alone, the next is read; a per-frame unsynchronised text
        Bytes body;
        put(body, frame24("TIT2", text_body(3, bytes_of("hidden")), 0x08));
        Bytes fz = text_body(0, latin1("a\xFF"));
        fz.push_back(0x00); put(fz, "b");                                            // a, FF 00, b -> "a" FF "b"
        put(body, frame24("TALB", fz, 0x02));
        const Bytes b = assemble(id3_tag(4, 0, body), mp3_frames(3));
        CHECK(meta_of(b, LAF_MP3, &m));
        CHECK(m.title[0] == '\0' && !strcmp(m.album, "a\xC3\xBF" "b"));
    }
    {   // TPE2 stands in for the artist, whichever comes first
        Bytes body;
        put(body, frame23("TPE2", text_body(0, latin1("Album Artist"))));
        const Bytes only = assemble(id3_tag(3, 0, body), mp3_frames(3));
        CHECK(meta_of(only, LAF_MP3, &m) && !strcmp(m.artist, "Album Artist"));
        put(body, frame23("TPE1", text_body(0, latin1("Track Artist"))));
        const Bytes both = assemble(id3_tag(3, 0, body), mp3_frames(3));
        CHECK(meta_of(both, LAF_MP3, &m) && !strcmp(m.artist, "Track Artist"));
        Bytes rev;
        put(rev, frame23("TPE1", text_body(0, latin1("Track Artist"))));
        put(rev, frame23("TPE2", text_body(0, latin1("Album Artist"))));
        const Bytes rb = assemble(id3_tag(3, 0, rev), mp3_frames(3));
        CHECK(meta_of(rb, LAF_MP3, &m) && !strcmp(m.artist, "Track Artist"));
    }
    {   // a tag whose size runs past the file; frames that run past the tag
        Bytes body;
        put(body, frame23("TIT2", text_body(0, latin1("Fine"))));
        Bytes tag = id3_tag(3, 0, body);
        Bytes lie = tag;
        lie[9] = 0x7F; lie[8] = 0x7F;
        put(lie, mp3_frames(3));
        CHECK(!meta_of(lie, LAF_MP3, &m));                                        // the audio is inside the "tag": no frame to find
        Bytes frame_lie = frame23("TIT2", text_body(0, latin1("Fine")));
        frame_lie[7] = 0x7F;
        const Bytes b2 = assemble(id3_tag(3, 0, frame_lie), mp3_frames(3));
        CHECK(meta_of(b2, LAF_MP3, &m) && m.title[0] == '\0');
    }
    {   // junk before the first frame, and a sync pattern in it that is not a frame
        Bytes audio = filler(200, 0x00);
        audio.push_back(0xFF); audio.push_back(0xFB); audio.push_back(0x00); audio.push_back(0x00);     // a bad header
        put(audio, filler(100, 0));
        const size_t junk = audio.size();
        put(audio, mp3_frames(200));
        CHECK(meta_of(audio, LAF_MP3, &m) && m.data_off == junk && m.sample_rate == 44100);
        CHECK(m.bitrate_kbps == 128 && m.duration_secs == 5 && !m.mp3_gapless);   // 200 frames of 417 bytes at 128 kbit/s
        CHECK(!meta_of(filler(5000, 0), LAF_MP3, &m));
    }
    {   // a Xing header without a LAME block: the frame count gives the length, nothing is trimmed; the Info frame is skipped
        Bytes frame = mp3_frames(1);
        const int xo = 4 + 32;
        memcpy(&frame[xo], "Xing", 4);
        frame[xo + 4] = 0; frame[xo + 5] = 0; frame[xo + 6] = 0; frame[xo + 7] = 0x01;      // flags: frames
        frame[xo + 8] = 0; frame[xo + 9] = 0; frame[xo + 10] = 0x03; frame[xo + 11] = 0xE8;  // 1000 frames
        Bytes b = frame;
        put(b, mp3_frames(5));
        CHECK(meta_of(b, LAF_MP3, &m));
        CHECK(m.total_frames == 1152000 && m.duration_secs == 26 && !m.mp3_gapless && m.mp3_skip == 0 && m.data_off == 417);
        // with LAME's delay and padding
        Bytes lame = frame;
        memcpy(&lame[xo + 12], "LAME3.99r", 9);
        const int d = 576, p = 1000;
        lame[xo + 12 + 21] = (uint8_t)(d >> 4); lame[xo + 12 + 22] = (uint8_t)(((d & 15) << 4) | (p >> 8)); lame[xo + 12 + 23] = (uint8_t)(p & 255);
        put(lame, mp3_frames(5));
        CHECK(meta_of(lame, LAF_MP3, &m));
        CHECK(m.mp3_gapless && m.mp3_skip == 576 + 529 && m.total_frames == 1152000 - 576 - 1000);
        // a delay and padding that are more than the file holds are not believed
        Bytes bad = frame;
        bad[xo + 10] = 0; bad[xo + 11] = 3;                                          // 3 frames = 3456 samples
        memcpy(&bad[xo + 12], "LAME3.99r", 9);
        bad[xo + 12 + 21] = 0x7D; bad[xo + 12 + 22] = 0x07; bad[xo + 12 + 23] = 0xD0;   // 2000 and 2000
        put(bad, mp3_frames(5));
        CHECK(meta_of(bad, LAF_MP3, &m) && !m.mp3_gapless && m.mp3_skip == 0 && m.total_frames == 3 * 1152);
    }
    {   // a VBRI header: bytes and frames after "VBRI" at 36
        Bytes frame = mp3_frames(1);
        memcpy(&frame[36], "VBRI", 4);
        frame[36 + 10] = 0; frame[36 + 11] = 0; frame[36 + 12] = 0x10; frame[36 + 13] = 0x00;     // bytes
        frame[36 + 14] = 0; frame[36 + 15] = 0; frame[36 + 16] = 0x00; frame[36 + 17] = 0x64;     // 100 frames
        Bytes b = frame;
        put(b, mp3_frames(4));
        CHECK(meta_of(b, LAF_MP3, &m) && m.total_frames == 115200 && m.duration_secs == 3 && m.data_off == 417);
    }
}

static void test_wav() {
    printf("- WAVE\n");
    LaMeta m;
    {
        const Bytes b = wav_file(1, 2, 44100, 16, 44100);
        CHECK(meta_of(b, LAF_WAV, &m));
        CHECK(m.sample_rate == 44100 && m.channels == 2 && m.bits == 16 && m.wav_format == 1 && m.block_align == 4);
        CHECK(m.total_frames == 44100 && m.duration_secs == 1 && m.data_off == 12 + 8 + 16 + 8 && m.data_len == 44100 * 4);
    }
    {   // every sample format the decoder knows
        const struct { int fmt, bits; } ok[] = { {1, 8}, {1, 16}, {1, 24}, {1, 32}, {3, 32}, {3, 64} };
        for (const auto &f : ok) CHECK(meta_of(wav_file(f.fmt, 2, 48000, f.bits, 100), LAF_WAV, &m) && m.wav_format == f.fmt && m.bits == f.bits);
        const struct { int fmt, bits; } bad[] = { {2, 16}, {1, 12}, {3, 16}, {0x55, 16} };
        for (const auto &f : bad) CHECK(!meta_of(wav_file(f.fmt, 2, 48000, f.bits, 100), LAF_WAV, &m));
        CHECK(!meta_of(wav_file(1, 9, 48000, 16, 100), LAF_WAV, &m));            // too many channels
        CHECK(!meta_of(wav_file(1, 0, 48000, 16, 100), LAF_WAV, &m));
        CHECK(!meta_of(wav_file(1, 2, 100, 16, 100), LAF_WAV, &m));              // a rate nobody uses
    }
    {   // WAVE_FORMAT_EXTENSIBLE: the sub-format's first two bytes say PCM or float
        Bytes f = wav_fmt(0xFFFE, 6, 48000, 24);
        le16(f, 22); le16(f, 24); le32(f, 0x3F);
        le16(f, 1); for (int i = 0; i < 14; i++) f.push_back(0);                // KSDATAFORMAT_SUBTYPE_PCM begins 01 00
        Bytes c;
        put(c, chunk("fmt ", f));
        put(c, chunk("data", filler(6 * 3 * 100, 0)));
        CHECK(meta_of(riff(c), LAF_WAV, &m) && m.wav_format == 1 && m.channels == 6 && m.bits == 24 && m.total_frames == 100);
        Bytes g = wav_fmt(0xFFFE, 2, 48000, 32);
        le16(g, 22); le16(g, 32); le32(g, 3);
        le16(g, 3); for (int i = 0; i < 14; i++) g.push_back(0);
        Bytes c2;
        put(c2, chunk("fmt ", g));
        put(c2, chunk("data", filler(8 * 50, 0)));
        CHECK(meta_of(riff(c2), LAF_WAV, &m) && m.wav_format == 3);
    }
    {   // LIST INFO tags; a chunk of odd length (padded); the data chunk after them
        Bytes info = bytes_of("INFO");
        put(info, chunk("INAM", bytes_of("Wave Title")));                      // 10 bytes
        put(info, chunk("IART", bytes_of("Odd")));                              // 3 bytes + a pad byte
        put(info, chunk("IPRD", bytes_of("Wave Album")));
        put(info, chunk("ITRK", bytes_of("9")));
        Bytes extra = chunk("LIST", info);
        put(extra, chunk("junk", filler(5, 1)));
        const Bytes b = wav_file(1, 2, 48000, 16, 480, extra);
        CHECK(meta_of(b, LAF_WAV, &m));
        CHECK(!strcmp(m.title, "Wave Title") && !strcmp(m.artist, "Odd") && !strcmp(m.album, "Wave Album") && m.track_no == 9);
        CHECK(m.total_frames == 480 && m.data_off == b.size() - 480 * 4);
    }
    {   // a data chunk that claims 0, or more than the file holds, is the rest of the file
        Bytes b = wav_file(1, 2, 48000, 16, 100);
        const size_t at = 12 + 8 + 16 + 4;
        b[at] = b[at + 1] = b[at + 2] = b[at + 3] = 0xFF;
        CHECK(meta_of(b, LAF_WAV, &m) && m.total_frames == 100);
        b[at] = b[at + 1] = b[at + 2] = b[at + 3] = 0;
        CHECK(meta_of(b, LAF_WAV, &m) && m.total_frames == 100);
    }
    {   // the data chunk before the format is not understood; a missing data chunk is not a file
        Bytes c;
        put(c, chunk("data", filler(100, 0)));
        put(c, chunk("fmt ", wav_fmt(1, 2, 48000, 16)));
        CHECK(!meta_of(riff(c), LAF_WAV, &m));
        CHECK(!meta_of(riff(chunk("fmt ", wav_fmt(1, 2, 48000, 16))), LAF_WAV, &m));
        CHECK(!meta_of(bytes_of("RIFF....AVI LIST0123456789"), LAF_WAV, &m));
    }
}

static void test_format_line() {
    printf("- the format line\n");
    LaMeta m;
    memset(&m, 0, sizeof m);
    char o[48];
    m.kind = LAF_FLAC; m.sample_rate = 44100; m.bits = 16;
    la_format_line(&m, o, sizeof o); CHECK(!strcmp(o, "FLAC 44.1 kHz / 16-bit"));
    m.sample_rate = 48000; m.bits = 24;
    la_format_line(&m, o, sizeof o); CHECK(!strcmp(o, "FLAC 48 kHz / 24-bit"));
    m.sample_rate = 192000;
    la_format_line(&m, o, sizeof o); CHECK(!strcmp(o, "FLAC 192 kHz / 24-bit"));
    m.kind = LAF_WAV; m.sample_rate = 22050; m.bits = 8;
    la_format_line(&m, o, sizeof o); CHECK(!strcmp(o, "WAV 22.05 kHz / 8-bit"));
    m.sample_rate = 11025;
    la_format_line(&m, o, sizeof o); CHECK(!strcmp(o, "WAV 11.025 kHz / 8-bit"));
    m.kind = LAF_MP3; m.bitrate_kbps = 192;
    la_format_line(&m, o, sizeof o); CHECK(!strcmp(o, "192 kbps MP3"));
    m.bitrate_kbps = 0;
    la_format_line(&m, o, sizeof o); CHECK(!strcmp(o, "MP3"));
    char small[6];
    m.kind = LAF_FLAC; m.sample_rate = 44100; m.bits = 16;
    la_format_line(&m, small, sizeof small); CHECK(strlen(small) == 5);
    la_format_line(&m, small, 0);
}

static void test_kinds() {
    printf("- names\n");
    CHECK(la_kind_of("a.flac") == LAF_FLAC && la_kind_of("A.FLAC") == LAF_FLAC && la_kind_of("x.Mp3") == LAF_MP3);
    CHECK(la_kind_of("x.WAV") == LAF_WAV && la_kind_of("noext") == LAF_NONE && la_kind_of("a.flacx") == LAF_NONE);
    CHECK(la_kind_of("a.mp3.txt") == LAF_NONE && la_kind_of("") == LAF_NONE && la_kind_of(NULL) == LAF_NONE && la_kind_of(".mp3") == LAF_MP3);
    CHECK(la_kind_of("dir.flac/file") == LAF_NONE);
}

// Every field of what comes back stays inside its bounds, whatever the file was.
static void check_invariants(const LaMeta &m, size_t size) {
    CHECK(memchr(m.title, 0, sizeof m.title) && memchr(m.artist, 0, sizeof m.artist) && memchr(m.album, 0, sizeof m.album));
    CHECK(memchr(m.pic_mime, 0, sizeof m.pic_mime));
    CHECK(m.pic_len == 0 || m.pic_off + m.pic_len <= size);
    CHECK(m.data_off <= size && m.data_off + m.data_len <= size);
    CHECK(m.track_no >= 0 && m.disc_no >= 0 && m.channels >= 0 && m.channels <= 8);
}

static void test_damage() {
    printf("- damaged files\n");
    const struct { const char *name; LaKind kind; } files[] = {
        { "chirp44.flac", LAF_FLAC }, { "chirp44_v24.mp3", LAF_MP3 }, { "chirp44_v23.mp3", LAF_MP3 },
        { "plain_v1.mp3", LAF_MP3 }, { "mono22.mp3", LAF_MP3 }, { "surround6.flac", LAF_FLAC },
    };
    uint32_t rng = 12345;
    auto next = [&]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
    for (const auto &f : files) {
        Bytes whole;
        if (!load(f.name, &whole)) { CHECK(false); continue; }
        // cut short at 60 lengths, concentrated at the start where the tags are
        for (int i = 0; i < 60; i++) {
            size_t n = i < 40 ? (size_t)i * 40 : whole.size() * (size_t)(i - 39) / 21;
            if (n > whole.size()) n = whole.size();
            Bytes cut(whole.begin(), whole.begin() + (long)n);
            LaMeta m;
            meta_of(cut, f.kind, &m);
            check_invariants(m, cut.size());
        }
        // bytes flipped in the first 20 KB (the tags and headers), 150 times
        for (int i = 0; i < 150; i++) {
            Bytes dmg = whole;
            const int flips = 1 + (int)(next() % 4);
            for (int k = 0; k < flips; k++) dmg[next() % (dmg.size() < 20000 ? dmg.size() : 20000)] = (uint8_t)next();
            LaMeta m;
            meta_of(dmg, f.kind, &m, (int)(next() % 3 == 0 ? 5 : 0));
            check_invariants(m, dmg.size());
        }
        // every byte of the first 300 set to 0xFF, in turn
        for (int i = 0; i < 300 && i < (int)whole.size(); i++) {
            Bytes dmg = whole;
            dmg[(size_t)i] = 0xFF;
            LaMeta m;
            meta_of(dmg, f.kind, &m);
            check_invariants(m, dmg.size());
        }
    }
    // the wrong kind of file
    Bytes flac;
    LaMeta m;
    CHECK(load("chirp44.flac", &flac));
    CHECK(!meta_of(flac, LAF_WAV, &m));
    Bytes mp3;
    CHECK(load("chirp44_v24.mp3", &mp3));
    CHECK(!meta_of(mp3, LAF_FLAC, &m) && !meta_of(mp3, LAF_WAV, &m));
    CHECK(!meta_of(Bytes(), LAF_MP3, &m) && !meta_of(Bytes(10, 0), LAF_FLAC, &m));
    CHECK(!la_read_meta(NULL, NULL, 100, LAF_FLAC, &m) && !meta_of(mp3, LAF_NONE, &m));
    // a reader that fails: the answer is a refusal, not a hang
    struct Fail { static int rd(void *, uint64_t, uint8_t *, int) { return -1; } };
    CHECK(!la_read_meta(Fail::rd, NULL, 100000, LAF_FLAC, &m) && !la_read_meta(Fail::rd, NULL, 100000, LAF_MP3, &m) && !la_read_meta(Fail::rd, NULL, 100000, LAF_WAV, &m));
}

int main() {
    test_flac_fixtures();
    test_flac_synthetic();
    test_mp3_fixtures();
    test_mp3_synthetic();
    test_wav();
    test_format_line();
    test_kinds();
    test_damage();
    printf("local tags: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
