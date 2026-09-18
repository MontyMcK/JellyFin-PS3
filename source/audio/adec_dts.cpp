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
#include "timing.h"
#include "jf_paths.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>   /* dca.h uses uint*_t but does not include this itself */
#include <stdint.h>
#include <stdbool.h>

extern "C" {
#include "dca/dca.h"
#include "dts_stream.h"
#include "dcahd_api.h"
#include "truehd_map.h"
}

#include <stdlib.h>

// How much extension-only data to tolerate before declaring the track
// coreless.  A DTS-HD MA frame is a few KB, so 512 KB is dozens of frames —
// far past the point where a core would have shown up — while still being a
// fraction of a second of playback, so the fallback reopen happens before the
// user has finished wondering why it is quiet.
#define DTS_NO_CORE_BYTES (512 * 1024)

static dca_state_t  *s_state = NULL;
static dts_stream_t  s_strm;          // static: audio path, no malloc
static bool          s_open         = false;

// ---- lossless path (vendored FFmpeg decoder, source/audio/dcahd/) --------
// libdca decodes the DTS CORE only, so on a DTS-HD MA track it throws away the
// lossless audio and keeps the lossy base.  When this decoder opens, the
// reader is switched to packet mode and whole frames come here instead.
// Allocated once at open (the audio path never allocates), released at close.
static dcahd_dec *s_hd       = NULL;
static void      *s_hd_mem   = NULL;
static void      *s_hd_plane = NULL;
static truehd_map_t s_hd_map;
static uint64_t     s_hd_mask   = 0;   // mask the map was built for
static int          s_hd_out_ch = 0;
static bool         s_hd_lossless = false;
static uint32_t     s_hd_frames = 0, s_hd_errors = 0;
static bool         s_hd_logged = false;

// Give up on the lossless decoder after this many consecutive failures and
// fall back to libdca's core.  A handful of errors right after a seek is
// normal; a stream it simply cannot handle should degrade to lossy audio
// rather than to silence.
#define HD_MAX_CONSEC_ERRORS 32
static uint32_t s_hd_consec_err = 0;

// Real-time budget.  XLL is far more expensive than the DTS core, and on this
// PPU it can cost more CPU than the audio it produces is long -- at which
// point it starves the display thread and the picture collapses to a few
// frames a second even with every buffer full.  Measured on hardware:
// everything buffered, nothing underrunning, and 1.2 fps.
//
// So the decoder measures itself.  Over a rolling window it compares time
// SPENT DECODING against the DURATION OF AUDIO PRODUCED; if decoding eats
// more than this share of real time there is not enough left for video, and
// the lossless path stands down in favour of libdca's core.  Lossy audio and
// a smooth picture beats lossless audio and a slideshow.
#define HD_BUDGET_PCT     70
#define HD_WINDOW_US      2000000ULL     // judge over 2 s of decoded audio
static u64 s_hd_dec_us = 0;              // CPU time spent decoding
static u64 s_hd_aud_us = 0;              // audio duration produced
static bool          s_logged_frame = false;
static bool          s_logged_ext   = false;
static uint32_t      s_logged_bad   = 0;

// Called by dts_stream for every decoded block, on the adec thread.
static void dts_emit(void *user, const float *frames, int n) {
    (void)user;
    adec_push_frames(frames, n);
}

// One complete DTS packet (core + extensions), on the adec thread.
static void dts_packet(void *user, const uint8_t *pkt, int len) {
    (void)user;
    if (!s_hd) return;

    int ch = 0, srate = 0, lossless = 0;
    uint64_t mask = 0;
    const u64 t0 = timing_get_us();
    int n = dcahd_api_decode(s_hd, pkt, len, &ch, &srate, &mask, &lossless);
    if (n <= 0) {
        if (n < 0 && ++s_hd_consec_err >= HD_MAX_CONSEC_ERRORS) {
            plog("adec_dts: lossless decoder failing, falling back to core");
            s_hd_errors++;
            s_strm.packet = NULL;          // back to libdca for the rest
        }
        return;
    }
    s_hd_consec_err = 0;
    s_hd_frames++;
    s_hd_lossless = lossless != 0;

    // Can we decode faster than the audio plays?  If not, give the CPU back.
    s_hd_dec_us += timing_get_us() - t0;
    s_hd_aud_us += (u64)n * 1000000ULL / (u64)(srate > 0 ? srate : 48000);
    if (s_hd_aud_us >= HD_WINDOW_US) {
        const unsigned pct = (unsigned)((s_hd_dec_us * 100ULL) / s_hd_aud_us);
        if (pct > HD_BUDGET_PCT) {
            char b[128];
            snprintf(b, sizeof(b),
                     "adec_dts: XLL costs %u%% of real time — too slow here,"
                     " falling back to the core", pct);
            plog(b);
            s_strm.packet = NULL;          // libdca takes over from here
            s_hd_lossless = false;
            return;
        }
        s_hd_dec_us = s_hd_aud_us = 0;     // rolling window
    }

    if (!s_hd_logged) {
        s_hd_logged = true;
        char b[128];
        snprintf(b, sizeof(b),
                 "adec_dts: HD decode hz=%d ch=%d mask=0x%llx %s",
                 srate, ch, (unsigned long long)mask,
                 lossless ? "LOSSLESS (XLL)" : "core only");
        plog(b);
    }

    // Rebuild the map only when the layout actually changes.
    if (mask != s_hd_mask) {
        if (truehd_map_build(mask, ch, s_hd_out_ch, &s_hd_map) < 0) {
            s_hd_mask = 0;
            return;                        // unusable layout: emit nothing
        }
        s_hd_mask = mask;
    }

    // Map in DMA-block-sized chunks rather than one big buffer: a frame can be
    // thousands of samples, and 256 frames is exactly what the ring wants.
    const int32_t *const *planes = dcahd_api_planes(s_hd);
    static float stage[256 * 8];           // adec thread only — no reentry
    const int32_t *slice[DCAHD_MAX_CHANNELS];
    for (int off = 0; off < n; off += 256) {
        int take = (n - off < 256) ? (n - off) : 256;
        for (int c = 0; c < ch && c < DCAHD_MAX_CHANNELS; c++)
            slice[c] = planes[c] + off;
        truehd_map_block_planar(slice, ch, take, &s_hd_map, stage);
        adec_push_frames(stage, take);
    }
}

static void hd_close(void) {
    if (s_hd) { dcahd_api_close(s_hd); s_hd = NULL; }
    free(s_hd_mem);   s_hd_mem   = NULL;
    free(s_hd_plane); s_hd_plane = NULL;
    s_hd_mask = 0; s_hd_lossless = false;
    s_hd_frames = s_hd_errors = s_hd_consec_err = 0;
    s_hd_logged = false;
}

// DTS-HD MA lossless decoding is ON by default.
//
// It was opt-in for a while because it produced audibly wrong output on the
// PPU while passing every host test, and the standing theory was a big-endian
// bug in the vendored XLL path that only hardware could find.  That theory was
// wrong.  x86-64 and big-endian PPC64 produced BYTE-IDENTICAL output, so the
// bug was host-reproducible all along -- it went unnoticed only because no
// host test fed the decoder an XLL stream.  test_dts_hd.c says in its own
// header that it cannot: there is no free DTS-HD MA encoder to build a fixture
// with.  But a fixture can be EXTRACTED from a disc rip instead of encoded,
// which is what tests/test_dts_xll_dump.c does.
//
// The cause was three bugs in the hand-written compat layer, not the decoder:
// zero-filled ff_log2_tab and ff_inverse tables, and an av_fast_mallocz that
// wiped the core's inter-frame ADPCM history.  See dcahd/PROVENANCE.md.
//
// Now verified bit-exact against ffmpeg on x86-64 AND on big-endian PPC64 --
// 806 of 806 frames of real 5.1 24-bit DTS-HD MA -- and confirmed working on
// the console.  Lossless is the point of this branch, so it is the default.
// Put "0" in /dev_hdd0/tmp/jellyfin_dtsma.txt to force libdca's lossy core.
#define DTSMA_FILE "jellyfin_dtsma.txt"
static bool hd_enabled(void) {
    FILE *f = fopen(jf_data_path(DTSMA_FILE), "r");
    if (!f) return true;              // no file: lossless, the default
    int v = 1;
    bool on = !(fscanf(f, "%d", &v) == 1 && v == 0);
    fclose(f);
    return on;
}

// Try to bring the lossless decoder up.  Failure is not fatal: the caller
// keeps libdca's core path, which is exactly what shipped before.
static bool hd_open(int out_ch) {
    if (!hd_enabled()) return false;
    s_hd_out_ch = out_ch;
    s_hd_mem    = malloc((size_t)dcahd_api_instance_size());
    s_hd_plane  = malloc(DCAHD_SAMPLE_BYTES);
    if (!s_hd_mem || !s_hd_plane) { hd_close(); return false; }
    s_hd = dcahd_api_open(s_hd_mem, s_hd_plane, (int)DCAHD_SAMPLE_BYTES);
    if (!s_hd) { hd_close(); return false; }
    return true;
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
    if (hd_open(out_ch)) {
        dts_stream_set_packet_mode(&s_strm, dts_packet, NULL);
        plog("adec_dts: lossless (XLL) decoder open");
    } else {
        plog(hd_enabled()
             ? "adec_dts: lossless decoder failed to open, using libdca core"
             : "adec_dts: lossless (XLL) disabled by setting, using libdca core");
    }
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
    hd_close();
    if (s_state) {
        dca_free(s_state);
        s_state = NULL;
    }
    memset(&s_strm, 0, sizeof(s_strm));
    s_open = false;
}

void adec_dts_reset(void) {
    if (s_open) dts_stream_reset(&s_strm);
    if (s_hd) { dcahd_api_flush(s_hd); s_hd_consec_err = 0; }
    s_hd_dec_us = s_hd_aud_us = 0;         // seek: restart the timing window
}

bool adec_dts_is_lossless(void) { return s_hd_lossless; }

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
                 " %s, ext=%u bytes",
                 s_strm.packet ? "decoding it LOSSLESSLY" : "decoding the core",
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
