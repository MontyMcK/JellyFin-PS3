// DTS decode path (libdca) — see adec_dts.h for the module contract.
//
// This file is the PS3 glue only: libdca lifetime, the output port width,
// logging, and pushing decoded frames into the shared PCM ring.  The actual
// elementary-stream reader — core sync scanning, partial frames across PES
// boundaries, and skipping DTS-HD / DTS:X extension substreams — lives in
// dts_stream.c, which is pure C so tests/test_dts_stream.c can drive it on
// the host.

#include "adec_dts.h"
#include "adec.h"
#include "plog.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>   /* dca.h uses uint*_t but does not include this itself */
#include <stdint.h>
#include <stdbool.h>

extern "C" {
#include "dca/dca.h"
#include "dts_stream.h"
}

// How much extension-only data to tolerate before declaring the track
// coreless.  A DTS-HD MA frame is a few KB, so 512 KB is dozens of frames —
// far past the point where a core would have shown up — while still being a
// fraction of a second of playback, so the fallback reopen happens before the
// user has finished wondering why it is quiet.
#define DTS_NO_CORE_BYTES (512 * 1024)

static dca_state_t  *s_state = NULL;
static dts_stream_t  s_strm;          // static: audio path, no malloc
static bool          s_open         = false;
static bool          s_logged_frame = false;
static bool          s_logged_ext   = false;
static uint32_t      s_logged_bad   = 0;

// Called by dts_stream for every decoded block, on the adec thread.
static void dts_emit(void *user, const float *frames, int n) {
    (void)user;
    adec_push_frames(frames, n);
}

bool adec_dts_open(int out_channels) {
    adec_dts_close();
    s_state = dca_init(0);   // no ASM accel: plain C on the PPU
    if (!s_state) {
        plog("adec_dts: dca_init failed");
        return false;
    }
    int out_ch = (out_channels == 6) ? 6 : 2;
    dts_stream_init(&s_strm, s_state, out_ch, dts_emit, NULL);
    s_open         = true;
    s_logged_frame = false;
    s_logged_ext   = false;
    s_logged_bad   = 0;
    char b[64];
    snprintf(b, sizeof(b), "adec_dts: open out_ch=%d req=0x%x",
             out_ch, s_strm.req_flags);
    plog(b);
    return true;
}

void adec_dts_close(void) {
    if (s_state) {
        dca_free(s_state);
        s_state = NULL;
    }
    memset(&s_strm, 0, sizeof(s_strm));
    s_open = false;
}

void adec_dts_reset(void) {
    if (s_open) dts_stream_reset(&s_strm);
}

bool adec_dts_saw_extension(void) {
    return s_open && s_strm.ext_count > 0;
}

bool adec_dts_no_core(void) {
    return s_open && s_strm.core_frames == 0 &&
           s_strm.ext_bytes >= DTS_NO_CORE_BYTES;
}

// Everything the decoder learned about the stream, reported once each.
static void log_stream_facts(void) {
    if (!s_logged_ext && s_strm.ext_count > 0) {
        s_logged_ext = true;
        char b[96];
        snprintf(b, sizeof(b),
                 "adec_dts: extension substream present (DTS-HD/DTS:X) —"
                 " decoding the core, ext=%u bytes",
                 (unsigned)s_strm.ext_bytes);
        plog(b);
    }
    if (!s_logged_frame && s_strm.have_first) {
        s_logged_frame = true;
        char b[128];
        snprintf(b, sizeof(b),
                 "adec_dts: hz=%d amode=0x%x lfe=%d bitrate=%d granted=0x%x "
                 "fsize=%d flen=%d blocks=%d",
                 s_strm.first_srate, s_strm.first_amode, s_strm.first_lfe,
                 s_strm.first_brate, s_strm.first_granted, s_strm.first_fsize,
                 s_strm.first_flen, s_strm.first_blocks);
        plog(b);
        if (s_strm.first_srate != 48000) {
            // The stream URL pins AudioSampleRate=48000, but a DTS track is
            // stream-COPIED, so the server does not resample it: a 96 kHz
            // DTS-HD core (rare, but legal) would play pitched down on the
            // fixed-48 kHz CellAudio port.  Loud log instead of silent
            // wrongness.
            snprintf(b, sizeof(b),
                     "adec_dts: WARNING stream is %d Hz, port is 48000 Hz",
                     s_strm.first_srate);
            plog(b);
        }
    }
    // Corrupt frames are normal in small numbers after a seek; a rising count
    // is the symptom worth seeing, so log it at widening intervals.
    if (s_strm.bad_frames >= s_logged_bad + 128) {
        s_logged_bad = s_strm.bad_frames;
        char b[64];
        snprintf(b, sizeof(b), "adec_dts: bad frames, total=%u",
                 (unsigned)s_strm.bad_frames);
        plog(b);
    }
}

void adec_dts_decode_payload(const u8 *es, int len) {
    if (!s_open || len <= 0) return;
    dts_stream_feed(&s_strm, (const uint8_t *)es, len);
    log_stream_facts();
}
