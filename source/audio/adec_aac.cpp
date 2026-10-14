// AAC decode path (libfaad) -- see adec_aac.h for the module contract.

#include "adec_aac.h"
#include "adec.h"
#include "adec_out.h"
#include "aac_adts.h"
#include "chan_map.h"
#include "plog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <neaacdec.h>

// The largest ADTS frame is 8191 bytes; the carry holds one and a partial successor.
#define AAC_CARRY_BYTES  (2 * 8192)
// One frame is 1024 samples a channel, 2048 with SBR.
#define AAC_MAX_FRAMES   2048

static NeAACDecHandle s_dec = NULL;
static bool     s_inited    = false;
static bool     s_src_mono  = false;        // the stream's headers say mono (libfaad then returns it as two identical channels)
static u8       s_carry[AAC_CARRY_BYTES];
static int      s_carry_len = 0;
static bool     s_logged    = false;
static u32      s_bad       = 0;

bool adec_aac_open(int out_channels) {
    adec_aac_close();
    s_dec = NeAACDecOpen();
    if (!s_dec) {
        plog("adec_aac: NeAACDecOpen failed");
        return false;
    }
    NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(s_dec);
    cfg->outputFormat = FAAD_FMT_FLOAT;
    cfg->downMatrix = 0;                       // the fold-down is chan_map's
    cfg->dontUpSampleImplicitSBR = 0;          // SBR output at its own (double) rate
    if (!NeAACDecSetConfiguration(s_dec, cfg)) {
        plog("adec_aac: NeAACDecSetConfiguration failed");
        NeAACDecClose(s_dec);
        s_dec = NULL;
        return false;
    }
    adec_out_open(out_channels);
    s_inited = false;
    s_src_mono = false;
    s_carry_len = 0;
    s_logged = false;
    s_bad = 0;
    char b[64];
    snprintf(b, sizeof(b), "adec_aac: open out_ch=%d caps=0x%lx", out_channels == 6 ? 6 : 2,
             (unsigned long)NeAACDecGetCapabilities());
    plog(b);
    return true;
}

void adec_aac_close(void) {
    if (s_dec) { NeAACDecClose(s_dec); s_dec = NULL; }
    adec_out_close();
    s_inited = false;
    s_carry_len = 0;
}

void adec_aac_reset(void) {
    s_carry_len = 0;
    if (s_dec && s_inited) NeAACDecPostSeekReset(s_dec, 0);
    adec_out_reset();
}

// One decoded frame: on to the output stage.
static void deliver(const NeAACDecFrameInfo &info, const float *pcm) {
    int ch = info.channels;
    int frames = ch > 0 ? (int)(info.samples / (unsigned long)ch) : 0;
    if (frames <= 0 || ch <= 0) return;
    if (frames > AAC_MAX_FRAMES) frames = AAC_MAX_FRAMES;

    // libfaad (built with parametric stereo, as both the console's and the host's are) decodes a mono stream as two
    // identical channels, so that PS can fill the second; without PS in use the first channel is the sound
    unsigned char pos1[1] = { CH_POS_FRONT_CENTER };
    const unsigned char *pos = info.channel_position;
    static float mono_buf[AAC_MAX_FRAMES];
    if (s_src_mono && ch == 2 && !info.ps) {
        for (int i = 0; i < frames; i++) mono_buf[i] = pcm[(size_t)i * 2];
        pcm = mono_buf;
        pos = pos1;
        ch = 1;
    }
    if (!s_logged) {
        s_logged = true;
        char b[112];
        snprintf(b, sizeof(b), "adec_aac: hz=%lu ch=%d sbr=%d ps=%d object=%d", (unsigned long)info.samplerate, ch,
                 (int)info.sbr, (int)info.ps, (int)info.object_type);
        plog(b);
    }
    // libfaad's float output is already scaled to +-1.0 (2.7 multiplies by 2^-15 in to_PCM_float, 2.11 likewise)
    adec_out_push(pcm, frames, ch, pos, (int)info.samplerate);
}

static void decode_carry(void) {
    int off = 0;
    while (s_carry_len - off >= 7) {
        const int fl = aac_adts_frame_len(s_carry + off, s_carry_len - off);
        if (fl == 0 || fl > 8191) { off++; continue; }                 // not a header: rescan
        if (s_carry_len - off < fl) break;                              // incomplete frame: wait
        if (!s_inited) {
            unsigned long rate = 0; unsigned char chans = 0;
            if (NeAACDecInit(s_dec, s_carry + off, (unsigned long)fl, &rate, &chans) < 0) { off++; continue; }
            AacConfig hc;
            s_src_mono = aac_parse_adts(s_carry + off, fl, &hc) && hc.channel_config == 1;
            s_inited = true;
        }
        NeAACDecFrameInfo info;
        void *pcm = NeAACDecDecode(s_dec, &info, s_carry + off, (unsigned long)fl);
        if (info.error || !pcm) {
            if ((s_bad++ % 128) == 0) {
                char b[96];
                snprintf(b, sizeof(b), "adec_aac: bad frame (%s), total=%u", NeAACDecGetErrorMessage(info.error), (unsigned)s_bad);
                plog(b);
            }
        } else {
            deliver(info, (const float *)pcm);
        }
        off += fl;
    }
    if (off > 0) {
        memmove(s_carry, s_carry + off, (size_t)(s_carry_len - off));
        s_carry_len -= off;
    }
}

void adec_aac_decode_payload(const u8 *es, int len) {
    if (!s_dec || len <= 0) return;
    while (len > 0) {
        int space = AAC_CARRY_BYTES - s_carry_len;
        if (space == 0) {
            // a full carry with no decodable frame is garbage: resync
            plog("adec_aac: carry overflow, resync");
            s_carry_len = 0;
            space = AAC_CARRY_BYTES;
        }
        const int take = len < space ? len : space;
        memcpy(s_carry + s_carry_len, es, (size_t)take);
        s_carry_len += take;
        es += take;
        len -= take;
        decode_carry();
    }
}
