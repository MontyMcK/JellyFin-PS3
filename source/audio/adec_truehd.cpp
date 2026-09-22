// TrueHD decode path — see adec_truehd.h for the module contract.
//
// This file is the PS3 glue only: decoder lifetime, output width, logging,
// and pushing decoded frames into the shared PCM ring.  The elementary-stream
// reader (access-unit framing, partial units across PES boundaries, resync)
// lives in truehd_stream.c and the channel map in truehd_map.c — both pure C,
// so tests/test_truehd_decode.c drives them on the host against FFmpeg's own
// decoder.

#include "adec_truehd.h"
#include "adec.h"
#include "plog.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

extern "C" {
#include "mlp_api.h"
#include "truehd_stream.h"
}

// The decoder instance lives in a static block: the audio path does not
// malloc (see docs/surround-5.1.md §6), and the vendored decoder's context is
// small enough — ~14 KB, checked at open against this buffer.  The stream
// reader (~27 KB, mostly its 16 KB carry buffer and one access unit of PCM)
// is static for the same reason.
#define TRUEHD_DEC_MEM 32768

static u8              s_dec_mem[TRUEHD_DEC_MEM];
static truehd_stream_t s_strm;
static bool            s_open         = false;
static bool            s_logged_first = false;
static u32             s_logged_bad   = 0;

// Called by truehd_stream for every decoded access unit, on the adec thread.
static void truehd_emit(void *user, const float *frames, int n) {
    (void)user;
    adec_push_frames(frames, n);
}

bool adec_truehd_open(int out_channels) {
    adec_truehd_close();
    int need = mlp_api_instance_size();
    if (need > TRUEHD_DEC_MEM) {
        char b[80];
        snprintf(b, sizeof(b), "adec_truehd: decoder needs %d bytes, have %d",
                 need, TRUEHD_DEC_MEM);
        plog(b);
        return false;
    }
    int out_ch = (out_channels == 8) ? 8 : (out_channels == 2) ? 2 : 6;
    if (!truehd_stream_init(&s_strm, s_dec_mem, out_ch, truehd_emit, NULL)) {
        plog("adec_truehd: decoder init failed");
        return false;
    }
    s_open         = true;
    s_logged_first = false;
    s_logged_bad   = 0;
    char b[64];
    snprintf(b, sizeof(b), "adec_truehd: open out_ch=%d", out_ch);
    plog(b);
    return true;
}

void adec_truehd_close(void) {
    memset(&s_strm, 0, sizeof(s_strm));
    s_open = false;
}

void adec_truehd_reset(void) {
    if (s_open) truehd_stream_reset(&s_strm);
}

int adec_truehd_program_channels(void) {
    return (s_open && s_strm.have_first) ? s_strm.first_channels : 0;
}

// How much unusable data to tolerate before giving up on the track.  A TrueHD
// access unit is at most a few KB and there are 1200 of them a second, so
// 512 KB without a single decoded unit is well past "the reader is still
// hunting for its first major sync" — while still being a fraction of a
// second, so the fallback reopen happens quickly.
#define TRUEHD_NO_AUDIO_BYTES (512 * 1024)

bool adec_truehd_no_audio(void) {
    return s_open && s_strm.units == 0 &&
           s_strm.fed_bytes >= TRUEHD_NO_AUDIO_BYTES;
}

static void log_stream_facts(void) {
    if (!s_logged_first && s_strm.have_first) {
        s_logged_first = true;
        char b[112];
        snprintf(b, sizeof(b),
                 "adec_truehd: hz=%d channels=%d mask=0x%llx units=%u",
                 s_strm.first_rate, s_strm.first_channels,
                 (unsigned long long)s_strm.first_mask,
                 (unsigned)s_strm.units);
        plog(b);
        if (s_strm.first_rate != 48000) {
            // A TrueHD track is stream-COPIED, so the server does not
            // resample it: a 96 or 192 kHz master (legal, and not rare on
            // music discs) would play pitched down on the fixed-48 kHz
            // CellAudio port.  Loud log instead of silent wrongness.
            snprintf(b, sizeof(b),
                     "adec_truehd: WARNING stream is %d Hz, port is 48000 Hz",
                     s_strm.first_rate);
            plog(b);
        }
    }
    // Failed units are normal in small numbers right after a seek (the reader
    // is hunting for a major sync); a rising count is the symptom worth
    // seeing, so log it at widening intervals.
    if (s_strm.bad_units >= s_logged_bad + 64) {
        s_logged_bad = s_strm.bad_units;
        char b[72];
        snprintf(b, sizeof(b), "adec_truehd: bad units=%u resync bytes=%u",
                 (unsigned)s_strm.bad_units, (unsigned)s_strm.resyncs);
        plog(b);
    }
}

void adec_truehd_decode_payload(const u8 *es, int len) {
    if (!s_open || len <= 0) return;
    truehd_stream_feed(&s_strm, (const uint8_t *)es, len);
    log_stream_facts();
}
