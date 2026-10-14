// FLAC frame decoder: see flac_dec.h.

#include "flac_dec.h"

#include <string.h>

// ---- CRCs --------------------------------------------------------------------------------------------

static uint8_t  s_crc8[256];
static uint16_t s_crc16[256];
static int      s_tables = 0;

static void make_tables(void) {
    for (int i = 0; i < 256; i++) {
        uint8_t c8 = (uint8_t)i;
        for (int k = 0; k < 8; k++) c8 = (uint8_t)((c8 & 0x80) ? (c8 << 1) ^ 0x07 : (c8 << 1));
        s_crc8[i] = c8;
        uint16_t c16 = (uint16_t)(i << 8);
        for (int k = 0; k < 8; k++) c16 = (uint16_t)((c16 & 0x8000) ? (c16 << 1) ^ 0x8005 : (c16 << 1));
        s_crc16[i] = c16;
    }
    s_tables = 1;
}

static uint8_t crc8(const uint8_t *p, int n) {
    uint8_t c = 0;
    for (int i = 0; i < n; i++) c = s_crc8[c ^ p[i]];
    return c;
}

static uint16_t crc16(const uint8_t *p, int n) {
    uint16_t c = 0;
    for (int i = 0; i < n; i++) c = (uint16_t)((c << 8) ^ s_crc16[(c >> 8) ^ p[i]]);
    return c;
}

// ---- bits -----------------------------------------------------------------------------------------------

typedef struct { const uint8_t *p; int len; long pos; int over; } Br;       // pos in bits

static uint32_t br_get(Br *b, int n) {
    if (n <= 0) return 0;
    if (b->pos + n > (long)b->len * 8) { b->over = 1; b->pos += n; return 0; }
    const long byte = b->pos >> 3;
    const int off = (int)(b->pos & 7);
    const int need = (off + n + 7) >> 3;                       // 1..5 bytes
    uint64_t v = 0;
    for (int i = 0; i < need; i++) v = (v << 8) | b->p[byte + i];
    v >>= (need * 8 - off - n);
    b->pos += n;
    return (uint32_t)(v & (n == 32 ? 0xFFFFFFFFull : ((1ull << n) - 1)));
}

static int32_t br_signed(Br *b, int n) {
    if (n <= 0) return 0;
    const uint32_t v = br_get(b, n);
    return (int32_t)(v << (32 - n)) >> (32 - n);
}

// zero bits before the next 1, which is consumed
static int br_unary(Br *b) {
    int count = 0;
    for (;;) {
        if (b->pos >= (long)b->len * 8) { b->over = 1; return count; }
        const long byte = b->pos >> 3;
        const int off = (int)(b->pos & 7);
        const unsigned cur = ((unsigned)b->p[byte] << off) & 0xFFu;      // what is left of this byte, left-aligned
        if (cur) {
            const int z = __builtin_clz(cur) - 24;
            b->pos += z + 1;
            return count + z;
        }
        count += 8 - off;
        b->pos += 8 - off;
        if (count > (1 << 26)) { b->over = 1; return count; }              // a runaway run of zeros
    }
}

// ---- header ----------------------------------------------------------------------------------------------

int flac_parse_header(const uint8_t *buf, int len, FlacInfo *info) {
    memset(info, 0, sizeof *info);
    if (len < 4) return len > 0 && memcmp(buf, "fLaC", (size_t)len) != 0 ? -1 : 0;
    if (memcmp(buf, "fLaC", 4) != 0) return -1;
    int pos = 4;
    bool have = false;
    for (;;) {
        if (pos + 4 > len) return 0;
        const int last = buf[pos] >> 7, type = buf[pos] & 0x7F;
        const int blen = (buf[pos + 1] << 16) | (buf[pos + 2] << 8) | buf[pos + 3];
        if (pos + 4 + blen > len) return 0;
        if (type == 0 && blen >= 34 && !have) {
            const uint8_t *s = buf + pos + 4;
            info->min_block = (s[0] << 8) | s[1];
            info->max_block = (s[2] << 8) | s[3];
            info->sample_rate = (s[10] << 12) | (s[11] << 4) | (s[12] >> 4);
            info->channels = ((s[12] >> 1) & 7) + 1;
            info->bps = (((s[12] & 1) << 4) | (s[13] >> 4)) + 1;
            info->total_samples = ((uint64_t)(s[13] & 0x0F) << 32) | ((uint64_t)s[14] << 24) | ((uint64_t)s[15] << 16) |
                                  ((uint64_t)s[16] << 8) | s[17];
            have = true;
        }
        pos += 4 + blen;
        if (last) break;
    }
    return have ? pos : -1;
}

// ---- subframes ---------------------------------------------------------------------------------------------

// The residual of a fixed or LPC subframe, into out[order..blocksize).  1 ok, 0 ran out of bytes, -1 invalid.
static int residual(Br *b, int32_t *out, int blocksize, int order) {
    const int method = (int)br_get(b, 2);
    if (method > 1) return -1;
    const int porder = (int)br_get(b, 4);
    const int nparts = 1 << porder;
    if (porder > 0 && (blocksize & (nparts - 1))) return -1;
    const int psize = blocksize >> porder;
    if (psize < order) return -1;
    const int pbits = method ? 5 : 4, esc = method ? 31 : 15;
    int idx = order;
    for (int p = 0; p < nparts; p++) {
        const int n = psize - (p == 0 ? order : 0);
        const int k = (int)br_get(b, pbits);
        if (b->over) return 0;
        if (k == esc) {
            const int bits = (int)br_get(b, 5);
            for (int i = 0; i < n; i++) out[idx++] = bits ? br_signed(b, bits) : 0;
        } else {
            for (int i = 0; i < n; i++) {
                const int q = br_unary(b);
                if (q > 0xFFFFFF) return -1;
                const uint32_t u = ((uint32_t)q << k) | br_get(b, k);
                out[idx++] = (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
            }
        }
        if (b->over) return 0;
    }
    return 1;
}

static int subframe(Br *b, int32_t *out, int bs, int bps) {
    if (br_get(b, 1) != 0) return -1;                         // the padding bit
    const int type = (int)br_get(b, 6);
    int wasted = 0;
    if (br_get(b, 1)) wasted = br_unary(b) + 1;
    if (b->over) return 0;
    const int bits = bps - wasted;
    if (bits <= 0 || bits > 25) return -1;

    if (type == 0) {                                          // constant
        const int32_t v = br_signed(b, bits);
        for (int i = 0; i < bs; i++) out[i] = v;
    } else if (type == 1) {                                   // verbatim
        for (int i = 0; i < bs; i++) out[i] = br_signed(b, bits);
    } else if (type >= 8 && type <= 15) {                     // fixed predictor
        const int order = type & 7;
        if (order > 4 || order > bs) return -1;
        for (int i = 0; i < order; i++) out[i] = br_signed(b, bits);
        const int r = residual(b, out, bs, order);
        if (r <= 0) return r;
        for (int i = order; i < bs; i++) {
            int64_t pred = 0;
            switch (order) {
            case 1: pred = out[i - 1]; break;
            case 2: pred = 2 * (int64_t)out[i - 1] - out[i - 2]; break;
            case 3: pred = 3 * (int64_t)out[i - 1] - 3 * (int64_t)out[i - 2] + out[i - 3]; break;
            case 4: pred = 4 * (int64_t)out[i - 1] - 6 * (int64_t)out[i - 2] + 4 * (int64_t)out[i - 3] - out[i - 4]; break;
            default: break;
            }
            out[i] = (int32_t)(pred + out[i]);
        }
    } else if (type >= 32) {                                  // LPC
        const int order = (type & 31) + 1;
        if (order > bs) return -1;
        for (int i = 0; i < order; i++) out[i] = br_signed(b, bits);
        const int prec = (int)br_get(b, 4) + 1;
        if (prec == 16) return -1;
        const int shift = br_signed(b, 5);
        if (shift < 0) return -1;
        int32_t coef[32];
        for (int j = 0; j < order; j++) coef[j] = br_signed(b, prec);
        if (b->over) return 0;
        const int r = residual(b, out, bs, order);
        if (r <= 0) return r;
        for (int i = order; i < bs; i++) {
            int64_t sum = 0;
            for (int j = 0; j < order; j++) sum += (int64_t)coef[j] * out[i - 1 - j];
            out[i] = (int32_t)((sum >> shift) + out[i]);
        }
    } else {
        return -1;
    }
    if (b->over) return 0;
    if (wasted) for (int i = 0; i < bs; i++) out[i] = (int32_t)((uint32_t)out[i] << wasted);
    return 1;
}

// ---- frames ------------------------------------------------------------------------------------------------------

int flac_decode_frame(const uint8_t *buf, int len, const FlacInfo *info, int32_t *const out[FLAC_MAX_CHANNELS],
                      int out_cap, FlacFrame *fr) {
    if (!s_tables) make_tables();
    if (len < 1) return 0;
    if (buf[0] != 0xFF) return -1;
    if (len < 2) return 0;
    if ((buf[1] & 0xFE) != 0xF8) return -1;
    if (len < 4) return 0;
    const int bs_code = buf[2] >> 4, sr_code = buf[2] & 15;
    const int ch_code = buf[3] >> 4, ss_code = (buf[3] >> 1) & 7;
    if (buf[3] & 1) return -1;

    // the coded frame / sample number: a UTF-8 style number of 1 to 7 bytes
    int pos = 4;
    const int lead = buf[pos];
    int extra;
    if (len <= pos) return 0;
    if ((lead & 0x80) == 0) extra = 0;
    else if ((lead & 0xE0) == 0xC0) extra = 1;
    else if ((lead & 0xF0) == 0xE0) extra = 2;
    else if ((lead & 0xF8) == 0xF0) extra = 3;
    else if ((lead & 0xFC) == 0xF8) extra = 4;
    else if ((lead & 0xFE) == 0xFC) extra = 5;
    else if (lead == 0xFE) extra = 6;
    else return -1;
    if (pos + 1 + extra > len) return 0;
    for (int i = 1; i <= extra; i++) if ((buf[pos + i] & 0xC0) != 0x80) return -1;
    uint64_t number = extra == 0 ? (uint64_t)lead : (uint64_t)(lead & (0x3F >> extra));
    for (int i = 1; i <= extra; i++) number = (number << 6) | (uint64_t)(buf[pos + i] & 0x3F);
    pos += 1 + extra;

    int blocksize;
    if (bs_code == 0) return -1;
    else if (bs_code == 1) blocksize = 192;
    else if (bs_code <= 5) blocksize = 576 << (bs_code - 2);
    else if (bs_code == 6) { if (pos + 1 > len) return 0; blocksize = buf[pos] + 1; pos += 1; }
    else if (bs_code == 7) { if (pos + 2 > len) return 0; blocksize = ((buf[pos] << 8) | buf[pos + 1]) + 1; pos += 2; }
    else blocksize = 256 << (bs_code - 8);

    static const int RATES[12] = { 0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000 };
    int rate;
    if (sr_code == 0) { if (!info || info->sample_rate <= 0) return -1; rate = info->sample_rate; }
    else if (sr_code <= 11) rate = RATES[sr_code];
    else if (sr_code == 12) { if (pos + 1 > len) return 0; rate = buf[pos] * 1000; pos += 1; }
    else if (sr_code == 13) { if (pos + 2 > len) return 0; rate = (buf[pos] << 8) | buf[pos + 1]; pos += 2; }
    else if (sr_code == 14) { if (pos + 2 > len) return 0; rate = ((buf[pos] << 8) | buf[pos + 1]) * 10; pos += 2; }
    else return -1;

    int bps;
    switch (ss_code) {
    case 0: if (!info || info->bps <= 0) return -1; bps = info->bps; break;
    case 1: bps = 8; break;
    case 2: bps = 12; break;
    case 4: bps = 16; break;
    case 5: bps = 20; break;
    case 6: bps = 24; break;
    default: return -1;
    }
    int channels;
    if (ch_code <= 7) channels = ch_code + 1;
    else if (ch_code <= 10) channels = 2;
    else return -1;

    if (pos + 1 > len) return 0;
    if (crc8(buf, pos) != buf[pos]) return -1;
    pos += 1;
    if (blocksize > out_cap || blocksize > FLAC_MAX_BLOCK) return -1;

    Br b = { buf, len, (long)pos * 8, 0 };
    for (int c = 0; c < channels; c++) {
        const bool side = (ch_code == 8 && c == 1) || (ch_code == 9 && c == 0) || (ch_code == 10 && c == 1);
        const int r = subframe(&b, out[c], blocksize, bps + (side ? 1 : 0));
        if (r <= 0) return r;
    }
    b.pos = (b.pos + 7) & ~7L;
    const int end = (int)(b.pos >> 3);
    if (end + 2 > len) return 0;
    if (crc16(buf, end) != (uint16_t)((buf[end] << 8) | buf[end + 1])) return -1;

    if (ch_code == 8) {                                       // left / side
        for (int i = 0; i < blocksize; i++) out[1][i] = out[0][i] - out[1][i];
    } else if (ch_code == 9) {                                // side / right
        for (int i = 0; i < blocksize; i++) out[0][i] = out[0][i] + out[1][i];
    } else if (ch_code == 10) {                               // mid / side
        for (int i = 0; i < blocksize; i++) {
            const int64_t mid = (int64_t)out[0][i] * 2 + (out[1][i] & 1);
            const int64_t side = out[1][i];
            out[0][i] = (int32_t)((mid + side) >> 1);
            out[1][i] = (int32_t)((mid - side) >> 1);
        }
    }
    fr->blocksize = blocksize;
    // A fixed-block stream numbers its frames; a variable one numbers its first samples.
    fr->sample_pos = (buf[1] & 1) ? number : number * (uint64_t)(info && info->max_block > 0 ? info->max_block : blocksize);
    fr->channels = channels;
    fr->bps = bps;
    fr->sample_rate = rate;
    return end + 2;
}
