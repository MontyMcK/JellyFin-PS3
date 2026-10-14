// AudioSpecificConfig and ADTS: see aac_adts.h.

#include "aac_adts.h"

#include <string.h>

static const int RATES[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
static const int CHANNELS[8] = { 0, 1, 2, 3, 4, 5, 6, 8 };

// MSB-first bit reader over the config bytes; reads past the end return zeros and set `over`.
typedef struct { const uint8_t *p; int len; int pos; bool over; } Bits;

static uint32_t bits_get(Bits *b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        const int byte = b->pos >> 3;
        if (byte >= b->len) { b->over = true; v <<= 1; b->pos++; continue; }
        v = (v << 1) | (uint32_t)((b->p[byte] >> (7 - (b->pos & 7))) & 1);
        b->pos++;
    }
    return v;
}

static int object_type(Bits *b) {
    int t = (int)bits_get(b, 5);
    if (t == 31) t = 32 + (int)bits_get(b, 6);
    return t;
}

bool aac_parse_asc(const uint8_t *asc, int len, AacConfig *out) {
    memset(out, 0, sizeof *out);
    if (!asc || len < 2) return false;
    Bits b = { asc, len, 0, false };
    int aot = object_type(&b);
    int sfi = (int)bits_get(&b, 4);
    int rate = 0;
    if (sfi == 15) {                               // an explicit rate: only if it is one ADTS can name
        rate = (int)bits_get(&b, 24);
        sfi = -1;
        for (int i = 0; i < 13; i++) if (RATES[i] == rate) sfi = i;
    } else if (sfi < 13) {
        rate = RATES[sfi];
    } else {
        return false;
    }
    const int cfg = (int)bits_get(&b, 4);
    if (aot == 5 || aot == 29) {                   // explicit SBR (or PS): the rate that follows is the output's; an LC core comes next
        const int esfi = (int)bits_get(&b, 4);
        if (esfi == 15) bits_get(&b, 24);
        aot = object_type(&b);
    }
    if (b.over) return false;
    if (sfi < 0 || aot < 1 || aot > 4) return false;
    if (cfg < 1 || cfg > 7) return false;
    out->object_type = aot;
    out->sf_index = sfi;
    out->sample_rate = rate;
    out->channel_config = cfg;
    out->channels = CHANNELS[cfg];
    return true;
}

bool aac_make_adts(const AacConfig *cfg, int raw_len, uint8_t hdr[7]) {
    const int frame_len = raw_len + 7;
    if (raw_len < 0 || frame_len > 8191) return false;
    const int profile = cfg->object_type - 1;
    hdr[0] = 0xFF;
    hdr[1] = 0xF1;                                  // sync, MPEG-4, layer 0, no CRC
    hdr[2] = (uint8_t)((profile << 6) | (cfg->sf_index << 2) | (cfg->channel_config >> 2));
    hdr[3] = (uint8_t)(((cfg->channel_config & 3) << 6) | (frame_len >> 11));
    hdr[4] = (uint8_t)((frame_len >> 3) & 0xFF);
    hdr[5] = (uint8_t)(((frame_len & 7) << 5) | 0x1F);      // buffer fullness: variable bit rate
    hdr[6] = 0xFC;                                  // (the low 6 bits of 0x7FF) and one raw data block
    return true;
}

int aac_adts_frame_len(const uint8_t *p, int avail) {
    if (avail < 7) return 0;
    if (p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) return 0;     // sync word and layer 0
    const int sf = (p[2] >> 2) & 0xF;
    if (sf >= 13) return 0;
    const int hdr = (p[1] & 1) ? 7 : 9;                      // protection_absent clear: a CRC follows
    const int len = ((p[3] & 3) << 11) | (p[4] << 3) | (p[5] >> 5);
    return len >= hdr ? len : 0;
}

bool aac_parse_adts(const uint8_t *p, int avail, AacConfig *out) {
    memset(out, 0, sizeof *out);
    if (aac_adts_frame_len(p, avail) == 0) return false;
    const int cfg = ((p[2] & 1) << 2) | (p[3] >> 6);
    if (cfg < 1 || cfg > 7) return false;
    out->object_type = ((p[2] >> 6) & 3) + 1;
    out->sf_index = (p[2] >> 2) & 0xF;
    out->sample_rate = RATES[out->sf_index];
    out->channel_config = cfg;
    out->channels = CHANNELS[cfg];
    return true;
}
