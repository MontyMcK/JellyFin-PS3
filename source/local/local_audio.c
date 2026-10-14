// Decoding a music file: see local_audio.h.

#include "local_audio.h"

#include <stdlib.h>
#include <string.h>

#include "chan_map.h"
#include "flac_dec.h"
#include "minimp3.h"
#include "resample.h"

#define SLICE        512                     // input frames mapped and converted at a time
#define FB_CAP_FLAC  (256 * 1024)            // a frame of 24-bit 8-channel audio at 4608 samples is ~110 KB
#define FB_CAP_OTHER (64 * 1024)
#define WAV_UNIT     1024                    // frames read at a time
#define MP3_AHEAD    16384                   // bytes buffered before a frame is decoded (minimp3 looks ahead for sync)

struct LaDecoder {
    LaRead   rd;
    void    *ctx;
    uint64_t size;
    LaMeta   m;
    uint64_t end;                            // the last byte of the audio + 1

    // the file, through a buffer
    uint8_t *fb;
    int      fb_cap, fb_len, fb_pos;
    uint64_t fb_off;                         // file offset of fb[0]
    bool     eof;                            // the buffer holds the rest of the audio
    bool     err;                            // a read failed

    // one decoded unit (a FLAC frame, an MP3 frame, a run of WAVE frames), interleaved floats at the file's rate
    float   *stage;
    int      stage_cap, stage_n, stage_pos;
    int      ch, rate;                       // of the stage
    unsigned char pos[8];                    // the stage's channel positions (chan_map.h)

    // to the port's format
    Resampler *rs;
    int      rs_rate;
    float   *fifo;
    int      fifo_cap, fifo_n, fifo_pos;

    uint64_t skip_in;                        // frames of the stage still to drop (the start of a seek, an MP3's delay)
    bool     limit_on;
    uint64_t limit_left;                     // frames still to play (an MP3 with a LAME header)

    // FLAC
    FlacInfo fi;
    int32_t *mem[FLAC_MAX_CHANNELS + 1];
    int32_t *planes[FLAC_MAX_CHANNELS];
    int      plane_cap;
    bool     seeking;                        // frames before seek_to are dropped
    uint64_t seek_to;
    uint32_t bad_frames;

    // MP3
    mp3dec_t dec;
};

// ---------------------------------------------------------------------------
//  The file buffer
// ---------------------------------------------------------------------------

// Makes at least `want` bytes available at fb_pos (fewer at the end of the audio, or after a failed read).
static void fb_fill(LaDecoder *d, int want) {
    if (d->fb_len - d->fb_pos >= want || d->eof) return;
    if (d->fb_pos > 0) {
        memmove(d->fb, d->fb + d->fb_pos, (size_t)(d->fb_len - d->fb_pos));
        d->fb_off += (uint64_t)d->fb_pos;
        d->fb_len -= d->fb_pos;
        d->fb_pos = 0;
    }
    while (d->fb_len < d->fb_cap && !d->eof) {
        const uint64_t at = d->fb_off + (uint64_t)d->fb_len;
        if (at >= d->end) { d->eof = true; break; }
        uint64_t room = (uint64_t)(d->fb_cap - d->fb_len);
        if (room > d->end - at) room = d->end - at;
        if (room > 65536) room = 65536;
        const int r = d->rd(d->ctx, at, d->fb + d->fb_len, (int)room);
        if (r < 0) { d->err = true; d->eof = true; break; }
        if (r == 0) { d->eof = true; break; }
        d->fb_len += r;
    }
}

// Starts buffering at a file offset (a seek).
static void fb_reset(LaDecoder *d, uint64_t at) {
    d->fb_off = at;
    d->fb_len = d->fb_pos = 0;
    d->eof = at >= d->end;
}

// ---------------------------------------------------------------------------
//  The output stage
// ---------------------------------------------------------------------------

static bool set_rate(LaDecoder *d, int rate) {
    if (d->rs && d->rs_rate == rate) return true;
    if (d->rs) { resample_close(d->rs); d->rs = NULL; }
    d->rs_rate = 0;
    if (rate != 48000) {
        d->rs = resample_open(rate, 2);
        if (!d->rs) return false;
        d->rs_rate = rate;
        const int need = resample_max_out(d->rs, SLICE);
        if (need > d->fifo_cap) {
            float *f = (float *)realloc(d->fifo, sizeof(float) * 2 * (size_t)need);
            if (!f) return false;
            d->fifo = f;
            d->fifo_cap = need;
        }
    }
    d->rate = rate;
    return true;
}

// One slice of the stage to the fifo.  False when nothing more can come.
static bool pull(LaDecoder *d, bool (*decode_unit)(LaDecoder *)) {
    for (;;) {
        if (d->stage_pos >= d->stage_n) {
            d->stage_pos = d->stage_n = 0;
            if (!decode_unit(d)) return false;
            if (d->stage_n == 0) continue;
        }
        int n = d->stage_n - d->stage_pos;
        if (n > SLICE) n = SLICE;
        if (d->skip_in > 0) {
            const int k = d->skip_in < (uint64_t)n ? (int)d->skip_in : n;
            d->stage_pos += k;
            d->skip_in -= (uint64_t)k;
            continue;
        }
        if (d->limit_on) {
            if (d->limit_left == 0) { d->stage_pos = d->stage_n; return false; }
            if ((uint64_t)n > d->limit_left) n = (int)d->limit_left;
            d->limit_left -= (uint64_t)n;
        }
        float mapped[SLICE * 2];
        chan_map_frames(d->stage + (size_t)d->stage_pos * (size_t)d->ch, n, d->ch, d->pos, mapped, 2);
        d->stage_pos += n;
        d->fifo_pos = 0;
        if (d->rate == 48000 || !d->rs) {
            memcpy(d->fifo, mapped, sizeof(float) * 2 * (size_t)n);
            d->fifo_n = n;
        } else {
            d->fifo_n = resample_process(d->rs, mapped, n, d->fifo, d->fifo_cap);
        }
        if (d->fifo_n > 0) return true;
    }
}

// ---------------------------------------------------------------------------
//  FLAC
// ---------------------------------------------------------------------------

static bool flac_unit(LaDecoder *d) {
    for (;;) {
        fb_fill(d, 4096);
        if (d->fb_len - d->fb_pos < 2) return false;
        // the next frame: from its sync code
        const uint8_t *b = d->fb + d->fb_pos;
        const int avail = d->fb_len - d->fb_pos;
        if (!(b[0] == 0xFF && (b[1] & 0xFE) == 0xF8)) {
            int i = 1;
            while (i + 1 < avail && !(b[i] == 0xFF && (b[i + 1] & 0xFE) == 0xF8)) i++;
            d->fb_pos += i;
            continue;
        }
        FlacFrame fr;
        const int r = flac_decode_frame(b, avail, &d->fi, d->planes, d->plane_cap, &fr);
        if (r == 0) {
            // the frame is not all here: more of the file, or the end of it (a cut-off last frame is dropped)
            if (d->eof) { d->fb_pos = d->fb_len; return false; }
            if (avail >= d->fb_cap) { d->fb_pos++; continue; }          // bigger than the buffer: not a frame
            fb_fill(d, avail + 1);
            if (d->fb_len - d->fb_pos <= avail) { d->fb_pos = d->fb_len; return false; }
            continue;
        }
        if (r < 0) {
            d->bad_frames++;
            d->fb_pos++;
            continue;
        }
        d->fb_pos += r;
        if (fr.channels != d->fi.channels) continue;                  // not this stream's
        if (d->seeking) {
            const uint64_t end = fr.sample_pos + (uint64_t)fr.blocksize;
            if (end <= d->seek_to) continue;                          // wholly before the place asked for
            d->seeking = false;
            d->skip_in = d->seek_to > fr.sample_pos ? d->seek_to - fr.sample_pos : 0;
        }
        const int ch = fr.channels;
        const float scale = 1.0f / (float)(1u << (fr.bps - 1));
        for (int i = 0; i < fr.blocksize; i++)
            for (int c = 0; c < ch; c++) d->stage[(size_t)i * (size_t)ch + (size_t)c] = (float)d->planes[c][i] * scale;
        d->stage_n = fr.blocksize;
        d->stage_pos = 0;
        return true;
    }
}

// The first frame at or after `from` that decodes, and its position; scans at most 128 KB.
static bool flac_locate(LaDecoder *d, uint64_t from, uint64_t *frame_off, uint64_t *sample_pos) {
    if (from >= d->end) return false;
    fb_reset(d, from);
    fb_fill(d, d->fb_cap);
    for (int i = 0; i + 2 < d->fb_len && i < 128 * 1024; i++) {
        const uint8_t *b = d->fb + i;
        if (b[0] != 0xFF || (b[1] & 0xFE) != 0xF8) continue;
        FlacFrame fr;
        const int r = flac_decode_frame(b, d->fb_len - i, &d->fi, d->planes, d->plane_cap, &fr);
        if (r > 0 && fr.channels == d->fi.channels) {
            *frame_off = from + (uint64_t)i;
            *sample_pos = fr.sample_pos;
            return true;
        }
    }
    return false;
}

static bool flac_seek(LaDecoder *d, uint64_t target) {
    uint64_t best = d->m.data_off;
    uint64_t lo = d->m.data_off, hi = d->end;
    for (int it = 0; it < 48 && hi - lo > 32768; it++) {
        const uint64_t mid = lo + (hi - lo) / 2;
        uint64_t off, sp;
        if (!flac_locate(d, mid, &off, &sp)) {
            if (d->err) return false;
            hi = mid;
            continue;
        }
        if (sp <= target) { best = off; lo = off; }
        else hi = mid;
    }
    fb_reset(d, best);
    d->seeking = target > 0;
    d->seek_to = target;
    return true;
}

// ---------------------------------------------------------------------------
//  MP3
// ---------------------------------------------------------------------------

static bool mp3_unit(LaDecoder *d) {
    for (;;) {
        fb_fill(d, MP3_AHEAD);
        const int avail = d->fb_len - d->fb_pos;
        if (avail <= 0) return false;
        short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        mp3dec_frame_info_t info;
        const int n = mp3dec_decode_frame(&d->dec, d->fb + d->fb_pos, avail, pcm, &info);
        if (info.frame_bytes <= 0) {
            // no frame in what is buffered: the end of the data, or junk
            if (d->eof) { d->fb_pos = d->fb_len; return false; }
            d->fb_pos += avail > MP3_AHEAD ? avail - MP3_AHEAD / 2 : 1;
            continue;
        }
        d->fb_pos += info.frame_bytes;
        if (n <= 0 || info.channels < 1 || info.channels > 2) continue;
        if (info.hz != d->rate && !set_rate(d, info.hz)) return false;
        d->ch = info.channels;
        chan_wave_positions(d->ch, d->pos);
        for (int i = 0; i < n * d->ch; i++) d->stage[i] = (float)pcm[i] * (1.0f / 32768.0f);
        d->stage_n = n;
        d->stage_pos = 0;
        return true;
    }
}

static bool mp3_seek(LaDecoder *d, uint32_t secs) {
    mp3dec_init(&d->dec);
    const LaMeta *m = &d->m;
    uint64_t byte = m->data_off;
    d->skip_in = 0;
    if (secs == 0) {
        d->skip_in = (uint64_t)m->mp3_skip;
    } else if (m->duration_secs > 0 && m->data_len > 0) {
        if (m->mp3_has_toc) {
            const double a = 100.0 * (double)secs / (double)m->duration_secs;
            int idx = (int)a;
            if (idx > 99) idx = 99;
            const double fa = m->mp3_toc[idx];
            const double fb = idx < 99 ? m->mp3_toc[idx + 1] : 256.0;
            const double f = fa + (fb - fa) * (a - (double)idx);
            byte += (uint64_t)((double)m->data_len * f / 256.0);
        } else {
            byte += (uint64_t)((double)m->data_len * (double)secs / (double)m->duration_secs);
        }
        // minimp3 makes no sound for the first frames after a jump (they lean on the bit reservoir of the ones
        // before): start three frames early so the music is where the seek asked for
        const uint64_t back = 3 * (uint64_t)m->bitrate_kbps * 125 * (uint64_t)m->mp3_spf / (uint64_t)m->sample_rate;
        byte = byte > m->data_off + back ? byte - back : m->data_off;
    }
    if (byte > d->end) byte = d->end;
    fb_reset(d, byte);
    if (d->limit_on) {
        const uint64_t at = (uint64_t)secs * (uint64_t)m->sample_rate;
        d->limit_left = m->total_frames > at ? m->total_frames - at : 0;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  WAVE
// ---------------------------------------------------------------------------

// A little-endian IEEE float or double, whatever the byte order of the machine.
static float le_f32(const uint8_t *p) {
    const uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}
static double le_f64(const uint8_t *p) {
    uint64_t u = 0;
    for (int i = 7; i >= 0; i--) u = (u << 8) | p[i];
    double f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static bool wav_unit(LaDecoder *d) {
    const int ba = d->m.block_align, ch = d->m.channels;
    fb_fill(d, WAV_UNIT * ba);
    int frames = (d->fb_len - d->fb_pos) / ba;
    if (frames > WAV_UNIT) frames = WAV_UNIT;
    if (frames <= 0) return false;
    const uint8_t *p = d->fb + d->fb_pos;
    d->fb_pos += frames * ba;
    const int bits = d->m.bits;
    const int n = frames * ch;
    if (d->m.wav_format == 3) {
        for (int i = 0; i < n; i++) {
            const float f = bits == 32 ? le_f32(p + (size_t)i * 4) : (float)le_f64(p + (size_t)i * 8);
            d->stage[i] = f > 2.0f ? 2.0f : f < -2.0f ? -2.0f : f == f ? f : 0.0f;       // (not a number: silence)
        }
    } else if (bits == 8) {
        for (int i = 0; i < n; i++) d->stage[i] = ((float)p[i] - 128.0f) * (1.0f / 128.0f);
    } else if (bits == 16) {
        for (int i = 0; i < n; i++) d->stage[i] = (float)(int16_t)(p[(size_t)i * 2] | (p[(size_t)i * 2 + 1] << 8)) * (1.0f / 32768.0f);
    } else if (bits == 24) {
        for (int i = 0; i < n; i++) {
            const int32_t v = (int32_t)(((uint32_t)p[(size_t)i * 3] << 8) | ((uint32_t)p[(size_t)i * 3 + 1] << 16) | ((uint32_t)p[(size_t)i * 3 + 2] << 24)) >> 8;
            d->stage[i] = (float)v * (1.0f / 8388608.0f);
        }
    } else {
        for (int i = 0; i < n; i++) {
            const int32_t v = (int32_t)((uint32_t)p[(size_t)i * 4] | ((uint32_t)p[(size_t)i * 4 + 1] << 8) | ((uint32_t)p[(size_t)i * 4 + 2] << 16) | ((uint32_t)p[(size_t)i * 4 + 3] << 24));
            d->stage[i] = (float)((double)v * (1.0 / 2147483648.0));
        }
    }
    d->stage_n = frames;
    d->stage_pos = 0;
    return true;
}

// ---------------------------------------------------------------------------
//  Public
// ---------------------------------------------------------------------------

bool la_can_decode(const LaMeta *m) {
    if (!m || m->kind == LAF_NONE || m->channels < 1 || m->channels > 8) return false;
    if (!resample_rate_supported(m->sample_rate)) return false;
    switch (m->kind) {
    case LAF_FLAC: return m->bits >= 4 && m->bits <= 24;
    case LAF_MP3:  return m->channels <= 2;
    case LAF_WAV:  return m->block_align > 0;
    default:      return false;
    }
}

void la_close(LaDecoder *d) {
    if (!d) return;
    if (d->rs) resample_close(d->rs);
    for (int c = 0; c <= FLAC_MAX_CHANNELS; c++) free(d->mem[c]);
    free(d->fb);
    free(d->stage);
    free(d->fifo);
    free(d);
}

LaDecoder *la_open(LaRead rd, void *ctx, uint64_t size, const LaMeta *m) {
    if (!rd || !la_can_decode(m)) return NULL;
    LaDecoder *d = (LaDecoder *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->rd = rd;
    d->ctx = ctx;
    d->size = size;
    d->m = *m;
    d->ch = m->channels;
    d->rate = 48000;
    d->end = m->kind == LAF_FLAC ? size : m->data_off + m->data_len;
    if (d->end > size) d->end = size;
    d->fb_cap = m->kind == LAF_FLAC ? FB_CAP_FLAC : FB_CAP_OTHER;
    d->fb = (uint8_t *)malloc((size_t)d->fb_cap);
    d->fifo_cap = SLICE;
    d->fifo = (float *)malloc(sizeof(float) * 2 * (size_t)d->fifo_cap);
    if (!d->fb || !d->fifo) { la_close(d); return NULL; }

    if (m->kind == LAF_FLAC) {
        d->fi.min_block = m->flac_min_block;
        d->fi.max_block = m->flac_max_block;
        d->fi.sample_rate = m->sample_rate;
        d->fi.channels = m->channels;
        d->fi.bps = m->bits;
        d->fi.total_samples = m->total_frames;
        // STREAMINFO says the largest block; a file that does not say (0, or under the format's 16) gets room for the largest there is
        d->plane_cap = m->flac_max_block >= 16 ? m->flac_max_block : FLAC_MAX_BLOCK;
        if (d->plane_cap < 1152) d->plane_cap = 1152;
        for (int c = 0; c <= m->channels; c++) {                       // one more: the scratch plane for a frame with extra channels
            d->mem[c] = (int32_t *)malloc(sizeof(int32_t) * (size_t)d->plane_cap);
            if (!d->mem[c]) { la_close(d); return NULL; }
        }
        for (int c = 0; c < FLAC_MAX_CHANNELS; c++) d->planes[c] = c < m->channels ? d->mem[c] : d->mem[m->channels];
        d->stage_cap = d->plane_cap * m->channels;
        chan_wave_positions(m->channels, d->pos);
        if (!set_rate(d, m->sample_rate)) { la_close(d); return NULL; }
    } else if (m->kind == LAF_MP3) {
        d->stage_cap = MINIMP3_MAX_SAMPLES_PER_FRAME;
        d->rate = 0;                                                    // the frame's own rate sets it
        d->limit_on = m->mp3_gapless && m->total_frames > 0;
    } else {
        d->stage_cap = WAV_UNIT * m->channels;
        chan_wave_positions(m->channels, d->pos);
        if (!set_rate(d, m->sample_rate)) { la_close(d); return NULL; }
    }
    d->stage = (float *)malloc(sizeof(float) * (size_t)d->stage_cap);
    if (!d->stage) { la_close(d); return NULL; }
    if (!la_seek(d, 0)) { la_close(d); return NULL; }
    return d;
}

bool la_seek(LaDecoder *d, uint32_t secs) {
    if (!d) return false;
    d->stage_n = d->stage_pos = 0;
    d->fifo_n = d->fifo_pos = 0;
    d->skip_in = 0;
    d->seeking = false;
    d->err = false;
    if (d->rs) resample_reset(d->rs);
    const LaMeta *m = &d->m;
    uint64_t frame = (uint64_t)secs * (uint64_t)m->sample_rate;
    if (m->total_frames > 0 && frame > m->total_frames) frame = m->total_frames;
    switch (m->kind) {
    case LAF_FLAC:
        if (frame == 0) { fb_reset(d, m->data_off); return true; }
        return flac_seek(d, frame);
    case LAF_MP3:
        return mp3_seek(d, secs);
    default: {
        uint64_t byte = m->data_off + frame * (uint64_t)m->block_align;
        if (byte > d->end) byte = d->end;
        fb_reset(d, byte);
        return true;
    }
    }
}

int la_decode(LaDecoder *d, float *lr, int max_pairs) {
    if (!d || !lr || max_pairs <= 0) return 0;
    bool (*unit)(LaDecoder *) = d->m.kind == LAF_FLAC ? flac_unit : d->m.kind == LAF_MP3 ? mp3_unit : wav_unit;
    int got = 0;
    while (got < max_pairs) {
        if (d->fifo_pos >= d->fifo_n) {
            d->fifo_pos = d->fifo_n = 0;
            if (!pull(d, unit)) break;
        }
        int n = d->fifo_n - d->fifo_pos;
        if (n > max_pairs - got) n = max_pairs - got;
        memcpy(lr + (size_t)got * 2, d->fifo + (size_t)d->fifo_pos * 2, sizeof(float) * 2 * (size_t)n);
        d->fifo_pos += n;
        got += n;
    }
    if (got == 0 && d->err) return -1;
    return got;
}

bool la_gapless_trimmed(const LaDecoder *d) {
    return d && !(d->m.kind == LAF_MP3 && !d->m.mp3_gapless);
}
