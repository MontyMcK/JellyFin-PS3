// Music playback engine — see music_player.h for the overview.
//
// The stream thread owns the whole per-track lifecycle: PlaybackInfo
// (session id + source container/bitrate for the meta line), the transcode
// stream, minimp3 decode, and Jellyfin playstate reporting.  Commands from
// the UI (pause aside, which is just a flag the pump thread honours) are
// polled between network reads, so skip/seek stay responsive even while the
// ring is full or the server is slow.
//
// The server is asked for 48 kHz MP3 (AudioSampleRate=48000) to match the
// audio port; a linear resampler guards the ring anyway in case a server
// ignores the parameter.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <net/net.h>
#include <sys/mutex.h>
#include <sys/thread.h>

#include "music_player.h"
#include "music_fft.h"
#include "music_sv.h"          // Canyon visualizer: stereo tap
#include "ui_wave_audio.h"
#include "minimp3.h"
#include "audio.h"
#include "stream.h"
#include "plog.h"
#include "jf_paths.h"           // jf_data_path(): the wave look-ahead file
extern void crash_log(const char *msg);   // survives a death plog does not
#include "timing.h"
#include "jellyfin_api.h"
#include "ui_internal.h"        // xmb_json_* helpers for the track fetch

extern u32 running;

// ---- PCM ring (interleaved float L/R pairs, power of two) ----
// 5.5 s at 48 kHz (2 MB).  It was 683 ms, and the gapless handover only
// starts once a track is fully decoded INTO this ring -- so the next track's
// five blocking server calls (stopped report, transcode stop, PlaybackInfo,
// playing report, and the stream open, which waits for the server to start
// a new transcode) had 683 ms at most before the ring ran dry: the gap.
#define MPCM_CAP 262144

// Encoder padding at a gapless handover: an MP3 transcode starts with a
// silent info frame (1152) and the encoder's delay (~1105), and ends padded
// to a whole frame.  Trimmed at the boundary only, only while the samples are
// below GAP_TRIM_THRESH (-60 dBFS -- a real quiet passage is far above the
// decoder's noise floor on digital silence), and never more than
// GAP_TRIM_MAX from each side.
#define GAP_TRIM_MAX    2400
#define GAP_TRIM_THRESH 1.0e-3f
static float        s_ring[MPCM_CAP * 2];
static int          s_wr = 0, s_rd = 0;
static volatile int s_n  = 0;
static u64          s_pushed_total = 0;   // pairs ever pushed since the last flush
static u64          s_read_total   = 0;   // pairs ever read since the last flush
// Pre-roll: a freshly started track is held until the ring has ~200 ms, so it
// starts clean instead of stuttering through its first network reads.
static volatile bool s_hold = false;
static int  s_trim_lead = 0;          // leading silence still to drop (gapless)
static bool s_gap_log   = false;      // log the queue at the handover's first samples
static sys_mutex_t  s_pcm_mtx;
static bool         s_pcm_mtx_ok = false;

// ---- queue + engine state ----
// Playback walks s_order (a permutation of 0..count-1) by position; shuffle
// just rewrites the permutation, so the queue overlay and Up Next always
// reflect the true play order.  s_order[s_pos] is the original track index.
static MusicTrack   s_queue[MUSIC_QUEUE_MAX];
static int          s_order[MUSIC_QUEUE_MAX];
static int          s_count  = 0;
static volatile int s_pos    = 0;      // the track being DECODED
static volatile int s_ui_pos = 0;      // the track being HEARD (what the UI shows)
static volatile bool s_shuffle = false;
static volatile bool s_run     = false;   // threads should keep going
static volatile bool s_active  = false;   // queue not yet finished
static volatile bool s_paused  = false;
static volatile bool s_started = false;   // music_start() .. music_stop()

// Commands (UI writes arg then cmd; stream thread consumes).
enum { MCMD_NONE = 0, MCMD_NEXT, MCMD_PREV, MCMD_SEEK, MCMD_JUMP, MCMD_STOP };
static volatile int s_cmd     = MCMD_NONE;
static volatile int s_cmd_arg = 0;

// Per-track playback position: elapsed = seek base + samples consumed.
static volatile u32 s_seek_base = 0;
static volatile u64 s_consumed  = 0;

// Current-track metadata for the meta line.
static u32  s_duration = 0;
static char s_src_info[40] = "";
static char s_session_id[80] = "";

// GAPLESS.  When a track's stream is fully downloaded and decoded, the next
// track is set up and decoded straight into the same ring behind it, instead
// of waiting for the ring to drain.  The handover is a boundary in the ring:
// once playback consumes past s_bnd_at, the heard track becomes s_bnd_pos
// (elapsed, duration and source line switch with it, sample-exact).
static bool s_bnd_pending = false;
static u64  s_bnd_at      = 0;
static int  s_bnd_pos     = 0;
static u32  s_bnd_dur     = 0;
static char s_bnd_src[40] = "";

static sys_ppu_thread_t s_stream_tid = 0;
static sys_ppu_thread_t s_pump_tid   = 0;
// music_stop() no longer waits for the stream thread (see there); the next
// music_start() joins it before touching any state it shares.
static bool             s_stream_unjoined = false;

// -------------------------------------------------------
// PCM ring + audio source callbacks
// -------------------------------------------------------

// ---- the wave's look-ahead ----
// The wave hears the ring s_wave_lead pairs AHEAD of the read cursor.  What it
// shows reaches the eye late: ~40 ms of hardware DMA after this tap, the
// frame's accumulation, a rebuild interval and the worker (~25-50 ms), then
// the flip -- ~100 ms all told, so a drum hit landed about a frame-and-a-half
// after it was heard.  Feeding it early by the same amount puts the hit on
// the beat.  jellyfin_wavelead.txt, in ms: absent = 100, 0 = the old tap
// (what is audible now), at most 400.  Read at music_start().
//
// s_wave_fed counts pairs handed to the wave, in s_read_total's coordinates:
// every pair is fed once, in order, never twice.  When the ring holds less
// than the lead (a track's first moments) it feeds what there is; if it ever
// falls behind the read cursor it feeds the pairs being read instead.
#define WAVELEAD_FILE    "jellyfin_wavelead.txt"
#define WAVELEAD_DEF_MS  100
#define WAVELEAD_CHUNK   16384                 // pairs per read call, at most
static int   s_wave_lead = WAVELEAD_DEF_MS * 48;
static u64   s_wave_fed  = 0;
static float s_wave_buf[WAVELEAD_CHUNK * 2];

static int wavelead_setting(void) {
    FILE *f = fopen(jf_data_path(WAVELEAD_FILE), "r");
    if (!f) return WAVELEAD_DEF_MS;
    int v = WAVELEAD_DEF_MS;
    if (fscanf(f, "%d", &v) != 1) v = WAVELEAD_DEF_MS;
    fclose(f);
    if (v < 0) v = 0;
    if (v > 400) v = 400;
    return v;
}

static void mring_flush(void) {
    sysMutexLock(s_pcm_mtx, 0);
    s_wr = s_rd = 0;
    s_n  = 0;
    s_pushed_total = s_read_total = 0;
    s_wave_fed = 0;
    s_bnd_pending = false;
    sysMutexUnlock(s_pcm_mtx);
}

static int mring_space(void) { return MPCM_CAP - s_n; }

static inline bool gap_quiet(float l, float r) {
    return (l < GAP_TRIM_THRESH && l > -GAP_TRIM_THRESH &&
            r < GAP_TRIM_THRESH && r > -GAP_TRIM_THRESH);
}

// Drop the old track's trailing padding from the NEWEST end of the ring (the
// reader takes from the oldest end, so this never touches what it is on).
static int mring_trim_tail(int max) {
    int dropped = 0;
    sysMutexLock(s_pcm_mtx, 0);
    while (dropped < max && s_n > 4096) {
        const int k = (s_wr - 1) & (MPCM_CAP - 1);
        if (!gap_quiet(s_ring[k * 2], s_ring[k * 2 + 1])) break;
        s_wr = k; s_n--; s_pushed_total--; dropped++;
    }
    sysMutexUnlock(s_pcm_mtx);
    return dropped;
}

static void mring_push(const float *lr, int n_pairs) {
    sysMutexLock(s_pcm_mtx, 0);
    for (int i = 0; i < n_pairs; i++) {
        if (s_n >= MPCM_CAP) break;
        if (s_trim_lead > 0) {                    // the new track's leading padding
            if (gap_quiet(lr[i * 2], lr[i * 2 + 1])) { s_trim_lead--; continue; }
            s_trim_lead = 0;
        }
        s_ring[s_wr * 2    ] = lr[i * 2    ];
        s_ring[s_wr * 2 + 1] = lr[i * 2 + 1];
        s_wr = (s_wr + 1) & (MPCM_CAP - 1);
        s_n++;
        s_pushed_total++;
    }
    sysMutexUnlock(s_pcm_mtx);
}

// Paused or pre-rolling: report nothing, and the paced writer plays silence.
static int music_pcm_avail(void) { return (s_paused || s_hold) ? 0 : s_n; }

// Music is stereo by design — the pluggable source contract reports frames
// of this fixed width (see audio_set_source in audio.h).
static int music_channels(void) { return 2; }

static int music_read_pcm(float *buf, int n_pairs) {
    sysMutexLock(s_pcm_mtx, 0);
    int got = 0;
    while (got < n_pairs && s_n > 0) {
        buf[got * 2    ] = s_ring[s_rd * 2    ];
        buf[got * 2 + 1] = s_ring[s_rd * 2 + 1];
        s_rd = (s_rd + 1) & (MPCM_CAP - 1);
        s_n--;
        got++;
    }
    if (s_bnd_pending && s_read_total + (u64)got >= s_bnd_at) {
        // Crossed into the next track: it is the one being heard now.
        s_consumed    = s_read_total + (u64)got - s_bnd_at;
        s_seek_base   = 0;
        s_ui_pos      = s_bnd_pos;
        s_duration    = s_bnd_dur;
        memcpy(s_src_info, s_bnd_src, sizeof(s_src_info));
        s_bnd_pending = false;
    } else {
        s_consumed += (u64)got;
    }
    const u64 rt0 = s_read_total;
    s_read_total += (u64)got;
    // The wave's share, gathered under the same lock (see WAVELEAD_FILE).
    int wn = 0;
    if (s_wave_lead > 0) {
        const u64 rt1 = s_read_total;
        if (s_wave_fed < rt0) s_wave_fed = rt0;
        while (s_wave_fed < rt1 && wn < WAVELEAD_CHUNK) {       // behind: what is heard now
            const int i = (int)(s_wave_fed - rt0);
            s_wave_buf[wn * 2] = buf[i * 2]; s_wave_buf[wn * 2 + 1] = buf[i * 2 + 1];
            wn++; s_wave_fed++;
        }
        u64 target = rt1 + (u64)s_wave_lead;
        if (target > rt1 + (u64)s_n) target = rt1 + (u64)s_n;
        while (s_wave_fed < target && wn < WAVELEAD_CHUNK) {    // ahead, from the ring
            const int k = (s_rd + (int)(s_wave_fed - rt1)) & (MPCM_CAP - 1);
            s_wave_buf[wn * 2] = s_ring[k * 2]; s_wave_buf[wn * 2 + 1] = s_ring[k * 2 + 1];
            wn++; s_wave_fed++;
        }
    }
    sysMutexUnlock(s_pcm_mtx);
    // Visualizer tap lives here, not at decode time: these samples hit the
    // hardware DMA ring (~40 ms of latency) now, while the decode cursor can
    // run ~700 ms ahead through the PCM ring — bars must move with what's
    // audible, not with what's buffered.
    music_viz_push(buf, got);
    // Third consumer: the Canyon visualizer wants L and R separately
    // (ui_canyon.cpp / sv_spectrum.h).  A copy into a ring, nothing more --
    // on the AUDIBLE tap, like the bars: Canyon applies its own look-ahead.
    music_sv_push(buf, got);
    // Second consumer: the XMB's background wave.  See
    // source/ui/render/ui_wave_audio.h.  Cheap (fourteen one-pole filters per
    // sample) and a no-op while the gate is off.  It hears the ring a little
    // AHEAD of what is audible (WAVELEAD_FILE above), so that what it shows
    // arrives on screen with the sound rather than after it.
    if (s_wave_lead > 0) wave_audio_push(s_wave_buf, wn);
    else                 wave_audio_push(buf, got);
    return got;
}

// -------------------------------------------------------
// Decode helpers
// -------------------------------------------------------

static mp3dec_t s_dec;

// Convert one decoded frame to float pairs, linear-resampling to 48 kHz if
// the server ignored AudioSampleRate.  Returns pairs written to out.
static int frame_to_48k(const short *pcm, int samples, int ch, int hz,
                        float *out, int out_cap) {
    if (samples <= 0) return 0;
    if (hz == 48000 || hz <= 0) {
        int n = samples > out_cap ? out_cap : samples;
        for (int i = 0; i < n; i++) {
            float l = pcm[i * ch] * (1.0f / 32768.0f);
            float r = (ch >= 2) ? pcm[i * ch + 1] * (1.0f / 32768.0f) : l;
            out[i * 2] = l;  out[i * 2 + 1] = r;
        }
        return n;
    }
    // Naive linear interpolation across this frame (no cross-frame state —
    // at worst a sub-sample discontinuity per frame, inaudible for a
    // fallback path that should never run).
    float ratio = (float)hz / 48000.0f;
    int   n_out = (int)((float)samples / ratio);
    if (n_out > out_cap) n_out = out_cap;
    for (int i = 0; i < n_out; i++) {
        float pos = (float)i * ratio;
        int   i0  = (int)pos;
        int   i1  = i0 + 1 < samples ? i0 + 1 : i0;
        float fr  = pos - (float)i0;
        float l0 = pcm[i0 * ch] * (1.0f / 32768.0f);
        float l1 = pcm[i1 * ch] * (1.0f / 32768.0f);
        float r0 = (ch >= 2) ? pcm[i0 * ch + 1] * (1.0f / 32768.0f) : l0;
        float r1 = (ch >= 2) ? pcm[i1 * ch + 1] * (1.0f / 32768.0f) : l1;
        out[i * 2]     = l0 + (l1 - l0) * fr;
        out[i * 2 + 1] = r0 + (r1 - r0) * fr;
    }
    return n_out;
}

// -------------------------------------------------------
// Track session setup
// -------------------------------------------------------

// Uppercase copy for the "320 kbps FLAC" display.
static void upper_copy(char *dst, int cap, const char *src) {
    int i = 0;
    for (; src[i] && i < cap - 1; i++)
        dst[i] = (src[i] >= 'a' && src[i] <= 'z') ? src[i] - 32 : src[i];
    dst[i] = '\0';
}

// PlaybackInfo for the track: session id, duration, and the SOURCE
// container/bitrate (responseBuffer still holds the reply — MediaSources[0]
// carries the first "Container"/"Bitrate" occurrences).
static void track_session_setup(const MusicTrack *t, u32 *dur_out, char *src_out) {
    char s_src_info[40] = "";           // shadows: filled here, committed by the caller
    u32  s_duration     = t->duration_secs;
    s_session_id[0] = '\0';

    unsigned total = 0;
    if (jellyfin_get_play_session_id(t->id, s_session_id,
                                     sizeof(s_session_id), &total)) {
        if (total > 0) s_duration = total;
        int len = (int)strlen(responseBuffer);
        char container[16] = "";
        xmb_json_str_range(responseBuffer, len, "Container",
                           container, sizeof(container));
        long long br = xmb_json_ll_range(responseBuffer, len, "Bitrate", 0);
        char cu[16] = "";
        if (container[0]) upper_copy(cu, sizeof(cu), container);
        if (br > 0 && cu[0])
            snprintf(s_src_info, sizeof(s_src_info), "%lld kbps %s",
                     br / 1000, cu);
        else if (cu[0])
            snprintf(s_src_info, sizeof(s_src_info), "%s", cu);
    }
    *dur_out = s_duration;
    memcpy(src_out, s_src_info, sizeof(s_src_info));
}

static void build_audio_url(char *url, int url_sz, const char *item_id,
                            u32 start_secs) {
    int n = snprintf(url, url_sz,
        "%s/Audio/%s/stream.mp3"
        "?AudioCodec=mp3"
        "&AudioBitrate=320000"
        "&AudioSampleRate=48000"
        "&MaxAudioChannels=2"
        "&Static=false"
        "&DeviceId=%s"
        "&StartTimeTicks=%llu",
        g_server, item_id, jf_device_id(),
        (unsigned long long)start_secs * 10000000ULL);
    if (s_session_id[0] && n > 0 && n < url_sz)
        snprintf(url + n, url_sz - n, "&PlaySessionId=%s", s_session_id);
}

// -------------------------------------------------------
// Stream thread — one track at a time
// -------------------------------------------------------

static int take_cmd(void) {
    int c = s_cmd;
    if (c != MCMD_NONE) s_cmd = MCMD_NONE;
    return c;
}

static u64 elapsed_ticks(void) {
    return ((u64)s_seek_base + s_consumed / 48000ULL) * 10000000ULL;
}

// Play the track at the current queue position from start_secs.  Returns
// the MCMD_* that ended it (MCMD_NEXT for natural end-of-track /
// unrecoverable stream errors).
//
// Every (re)start gets a FRESH PlaybackInfo session — including seeks.
// Reusing the session across a seek looked like a free optimization, but
// Jellyfin then re-attaches the request to the already-running audio
// transcode (which began at offset 0) and StartTimeTicks is ignored: the
// track audibly restarts from 0:00.  The kill via /Videos/ActiveEncodings
// doesn't reliably take for audio jobs the way it does for video.
static bool s_natural_end = false;   // the last track ended by running out, fully decoded

static int play_one_track(u32 start_secs, bool gapless) {
    if (!s_run) return MCMD_STOP;
    crash_log("m0 track start");
    const MusicTrack *t = &s_queue[s_order[s_pos]];
    s_natural_end = false;

    mp3dec_init(&s_dec);
    if (!gapless) {
        mring_flush();
        music_viz_reset();
        music_sv_reset();
        sysMutexLock(s_pcm_mtx, 0);
        s_consumed  = 0;
        s_seek_base = start_secs;
        s_ui_pos    = s_pos;
        s_duration  = t->duration_secs;
        s_src_info[0] = '\0';
        sysMutexUnlock(s_pcm_mtx);
        s_hold = true;
        s_trim_lead = 0;
        s_gap_log   = false;
    } else {
        // Everything of the previous track is in the ring already: this one
        // starts where it ends -- minus both tracks' encoder padding.
        const int tail = mring_trim_tail(GAP_TRIM_MAX);
        s_trim_lead = GAP_TRIM_MAX;
        s_gap_log   = true;
        {
            char b[96];
            snprintf(b, sizeof b, "music: gapless handover, %d ms queued, %d padding samples trimmed from the tail",
                     (int)((long long)s_n * 1000 / 48000), tail);
            plog(b);
        }
        sysMutexLock(s_pcm_mtx, 0);
        s_bnd_at      = s_pushed_total;
        s_bnd_pos     = s_pos;
        s_bnd_dur     = t->duration_secs;
        s_bnd_src[0]  = '\0';
        s_bnd_pending = true;
        sysMutexUnlock(s_pcm_mtx);
    }

    char buf[160];
    snprintf(buf, sizeof(buf), "music: track %d/%d start=%us id=%.16s%s",
             s_pos + 1, s_count, start_secs, t->id, gapless ? " (gapless)" : "");
    plog(buf);

    {
        u32  dur = t->duration_secs;
        char src[40] = "";
        track_session_setup(t, &dur, src);
        sysMutexLock(s_pcm_mtx, 0);
        if (s_bnd_pending && s_bnd_pos == s_pos) {
            s_bnd_dur = dur;
            memcpy(s_bnd_src, src, sizeof(s_bnd_src));
        } else if (s_ui_pos == s_pos) {
            s_duration = dur;
            memcpy(s_src_info, src, sizeof(s_src_info));
        }
        sysMutexUnlock(s_pcm_mtx);
    }
    // Each of these is a blocking round trip.  A stop requested meanwhile
    // (the user left the screen) ends the track here rather than starting a
    // stream nobody will hear.
    if (!s_run) { crash_log("m0x stopped before stream"); return MCMD_STOP; }
    jellyfin_report_playing(t->id, s_session_id,
                            (u64)start_secs * 10000000ULL);
    if (!s_run) { crash_log("m0x stopped before stream"); return MCMD_STOP; }
    crash_log("m0b stream_open");

    char url[1024];
    build_audio_url(url, sizeof(url), t->id, start_secs);
    int sock = stream_open(url);
    if (sock < 0) {
        plog("music: stream_open failed, skipping track");
        jellyfin_report_stopped(t->id, s_session_id, elapsed_ticks());
        usleep(400000);       // don't machine-gun through a dead server
        return MCMD_NEXT;
    }

    // MP3 byte buffer: reads append at buf_len, decode consumes at buf_pos,
    // leftovers slide back to the front when the tail runs out of room.
    // 256 KB, ~6.5 s at 320 kbps: when the download finishes this is what
    // covers setting up the next track without a gap.
    static u8 mp3[262144];
    #define MP3_MIN_AHEAD 16384
    int  buf_pos = 0, buf_len = 0;
    bool eof = false;
    u64  last_prog_us = timing_get_us();
    int  ret = MCMD_NONE;
    // Tempo diagnosis ("the first second is sped up" after an Up Next jump):
    // the stream's real format, and how much actually played in the first
    // three seconds -- 144000 frames at 48 kHz means real time.
    const u64 diag_t0 = timing_get_us();
    const u64 diag_c0 = s_consumed;
    bool diag_fmt = false, diag_rate = false;

    while (running && s_run) {
        int c = take_cmd();
        if (c != MCMD_NONE) { ret = c; break; }

        // ~10 s progress heartbeat keeps the server's session view honest.
        //
        // ASYNC, and it has to be: this thread is the only thing refilling a
        // ring that holds 683 ms of audio, and the blocking version of this
        // call stalled it for a whole server round trip every ten seconds.
        // That is the dropout you could hear.
        u64 now = timing_get_us();
        if (!diag_rate && now - diag_t0 >= 3000000ULL) {
            char d[112];
            snprintf(d, sizeof d, "music: first 3 s played %llu frames (real time = 144000)%s",
                     (unsigned long long)(s_consumed - diag_c0), gapless ? " [gapless]" : "");
            plog(d);
            diag_rate = true;
        }
        if (now - last_prog_us >= 10000000ULL) {
            last_prog_us = now;
            jellyfin_report_progress_async(t->id, s_session_id, elapsed_ticks(),
                                           s_paused);
        }

        // Fill: keep a healthy sync window ahead of the decoder.  A read
        // timeout (rd == 0) falls through to decode whatever is buffered so
        // a slow server can't starve the ring while data sits undecoded.
        if (!eof && buf_len < (int)sizeof(mp3) - 188) {
            int rd = stream_read(sock, mp3 + buf_len, 188);
            if (rd < 0) eof = true;
            else if (rd > 0) {
                buf_len += 188;
                if (buf_len - buf_pos < MP3_MIN_AHEAD) continue;   // buffer more first
            }
        }

        // Decode while there's data and the ring has room for a frame.
        bool progressed = false;
        // WHOLE FRAMES ONLY.  minimp3 treats a frame cut off by the end of
        // the buffer as junk: it skips it and resyncs, losing audio.  The
        // loop used to decode right up to the last buffered byte after every
        // 188-byte read, which at the start of a track (ring empty) dropped
        // ~18% of the frames -- measured on the server's own transcode: 150.3 s
        // decoded of a 184.6 s track.  The music jumped forward: the
        // "sped-up first second" after a skip or an Up Next pick.  Keep
        // MP3_MIN_AHEAD buffered (many frames, and minimp3's sync lookahead)
        // until the stream has ended; with it the same file decodes to the
        // exact sample.
        while (buf_len - buf_pos >= (eof ? 1 : MP3_MIN_AHEAD)) {
            if (mring_space() < 1300) break;   // ring nearly full
            mp3dec_frame_info_t info;
            short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
            int samples = mp3dec_decode_frame(&s_dec, mp3 + buf_pos,
                                              buf_len - buf_pos, pcm, &info);
            if (info.frame_bytes <= 0) break;  // needs more bytes
            buf_pos += info.frame_bytes;
            progressed = true;
            if (!diag_fmt && samples > 0) {
                char d[112];
                snprintf(d, sizeof d, "music: stream %d Hz, %d ch, %d kbps, first frame after %llu ms",
                         info.hz, info.channels, info.bitrate_kbps,
                         (unsigned long long)((timing_get_us() - diag_t0) / 1000));
                plog(d);
                diag_fmt = true;
            }
            if (samples > 0) {
                static float out[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
                int pairs = frame_to_48k(pcm, samples, info.channels, info.hz,
                                         out, MINIMP3_MAX_SAMPLES_PER_FRAME);
                mring_push(out, pairs);
                if (s_gap_log && s_trim_lead == 0) {
                    char b[80];
                    snprintf(b, sizeof b, "music: next track's audio arrived with %d ms still queued",
                             (int)((long long)s_n * 1000 / 48000));
                    plog(b);
                    s_gap_log = false;
                }
            }
        }

        // Slide leftovers down once the consumed prefix gets large.
        if (buf_pos > (int)sizeof(mp3) / 2) {
            memmove(mp3, mp3 + buf_pos, buf_len - buf_pos);
            buf_len -= buf_pos;
            buf_pos  = 0;
        }

        if (s_hold && (s_n >= 9600 || eof)) s_hold = false;   // ~200 ms pre-roll

        if (eof && !progressed && buf_pos < buf_len && mring_space() >= 1300)
            buf_pos = buf_len;                 // trailing bytes that are not a frame
        if (eof && buf_pos >= buf_len && s_pos + 1 < s_count) {
            // Fully decoded with a track after it: hand over now, while the
            // ring still plays this one out (gapless).
            s_natural_end = true;
            ret = MCMD_NEXT;
            break;
        }
        if (eof) {
            // Drained when less than one DMA block (256 samples) remains:
            // the pump can never consume a final partial block (it writes
            // silence instead), so waiting for exactly 0 would hang here
            // on any track whose sample count isn't block-aligned — which
            // is every other track (1152-sample MP3 frames).
            if (buf_pos >= buf_len && s_n < 256) { ret = MCMD_NEXT; break; }
            if (!progressed && buf_pos >= buf_len) {
                // Bytes exhausted, ring draining — wait it out.
                usleep(10000);
            }
        } else if (!progressed && mring_space() < 1300) {
            usleep(10000);                     // ring full: let the DMA drain
        } else if (!progressed && buf_len - buf_pos > 60000) {
            // A near-full buffer the decoder can't advance through means the
            // stream isn't MP3 (error page, wrong container) — skip out
            // instead of spinning on it forever.
            plog("music: undecodable stream, skipping track");
            ret = MCMD_NEXT;
            break;
        }
    }
    if (ret == MCMD_NONE) ret = MCMD_STOP;     // app quit / engine stop

    // CRASH MARKERS, not plog: plog queues into a ring that a writer thread
    // drains, so the last lines before a death are exactly the ones that never
    // reach the file -- which is why the last crash could be narrowed to "in
    // here somewhere" and no further. crash_log() writes immediately.
    //
    // This is the sequence a seek runs too, four blocking HTTP calls deep,
    // which is where the app died with a "stopped" report already delivered
    // and no line after it.
    crash_log("m1 track end: netClose");
    netClose(sock);
    crash_log("m2 report_stopped");
    jellyfin_report_stopped(t->id, s_session_id, elapsed_ticks());
    crash_log("m3 stop_transcode");
    jellyfin_stop_transcode(s_session_id);
    crash_log("m4 track end done");
    return ret;
}

static void music_stream_thread(void *arg) {
    (void)arg;
    u32 start_secs = 0;
    bool gapless = false;
    while (running && s_run) {
        int end = play_one_track(start_secs, gapless);
        start_secs = 0;
        gapless = false;
        if (!s_run || end == MCMD_STOP) break;
        if (end == MCMD_NEXT && s_natural_end) {
            if (s_pos + 1 < s_count) { s_pos = s_pos + 1; gapless = true; continue; }
            break;
        }
        // A user command acts on the track being HEARD.  If the next one was
        // already being decoded behind it, drop that and start from the heard
        // track's position.
        sysMutexLock(s_pcm_mtx, 0);
        if (s_bnd_pending) s_bnd_pending = false;
        s_pos = s_ui_pos;
        sysMutexUnlock(s_pcm_mtx);
        if (end == MCMD_NEXT) {
            if (s_pos + 1 < s_count) s_pos = s_pos + 1;
            else break;                        // queue finished
        } else if (end == MCMD_PREV) {
            if (s_pos > 0) s_pos = s_pos - 1;
        } else if (end == MCMD_SEEK) {
            start_secs = (u32)s_cmd_arg;
        } else if (end == MCMD_JUMP) {
            int pos = s_cmd_arg;
            if (pos < 0) pos = 0;
            if (pos >= s_count) pos = s_count - 1;
            s_pos = pos;
        }
    }
    s_active = false;
    // A stop's cancel flag was for THIS thread's stream_open; nothing else
    // may inherit it (the video player's stream_open reads the same flag).
    if (!s_run) g_stream_cancel = false;
    plog("music: stream thread exit");
    crash_log("m9 stream thread exit");
    sysThreadExit(0);
}

// -------------------------------------------------------
// Pump thread — services the audio port DMA from the ring
// -------------------------------------------------------

static void music_pump_thread(void *arg) {
    (void)arg;
    while (running && s_run) {
        // Always serviced, even paused or finished: the paced writer then
        // plays silence instead of the hardware looping its last 40 ms.
        if (!audio_write_pcm())
            usleep(1000);
    }
    plog("music: pump thread exit");
    sysThreadExit(0);
}

// -------------------------------------------------------
// Public API
// -------------------------------------------------------

bool music_start(const MusicTrack *tracks, int count, int start_idx) {
    if (s_started || count <= 0) return false;
    // The previous session's stream thread may still be finishing a network
    // call it was in when that screen closed; it shares every static below.
    if (s_stream_unjoined) {
        u64 r;
        sysThreadJoin(s_stream_tid, &r);
        s_stream_unjoined = false;
    }
    g_stream_cancel = false;
    if (count > MUSIC_QUEUE_MAX) count = MUSIC_QUEUE_MAX;
    if (start_idx < 0)      start_idx = 0;
    if (start_idx >= count) start_idx = count - 1;

    if (!s_pcm_mtx_ok) {
        sys_mutex_attr_t mattr;
        sysMutexAttrInitialize(mattr);
        sysMutexCreate(&s_pcm_mtx, &mattr);
        s_pcm_mtx_ok = true;
    }

    memcpy(s_queue, tracks, (size_t)count * sizeof(MusicTrack));
    s_count     = count;
    for (int i = 0; i < count; i++) s_order[i] = i;
    s_pos       = start_idx;
    s_ui_pos    = start_idx;
    s_hold      = true;
    s_shuffle   = false;
    {
        const int ms = wavelead_setting();
        s_wave_lead = ms * 48;             // 48 kHz pairs
        char b[80];
        snprintf(b, sizeof b, "music: wave look-ahead %d ms (%s)", ms, WAVELEAD_FILE);
        plog(b);
    }
    srand((unsigned)timing_get_us());
    s_cmd       = MCMD_NONE;
    s_paused    = false;
    s_seek_base = 0;
    s_consumed  = 0;
    s_duration  = 0;
    s_src_info[0] = '\0';
    mring_flush();
    music_viz_reset();
    music_sv_reset();

    audio_set_source(music_pcm_avail, music_read_pcm, music_channels);
    audio_open(2);   // music path is stereo by design
    audio_set_paced(true);

    s_run     = true;
    s_active  = true;
    s_started = true;
    // Before the threads exist, so neither of the two callers can race it up.
    jellyfin_report_init();
    sysThreadCreate(&s_pump_tid, music_pump_thread, NULL,
                    700, 0x8000, THREAD_JOINABLE, (char*)"jf_mpump");
    sysThreadCreate(&s_stream_tid, music_stream_thread, NULL,
                    850, 0x20000, THREAD_JOINABLE, (char*)"jf_music");
    return true;
}

// The video player calls this before it opens a stream: a music session that
// was closed mid-network-call may still own its thread and the shared
// stream_open cancel flag.  Blocks only in that rare case.
void music_join_stale(void) {
    if (s_started || !s_stream_unjoined) return;
    u64 r;
    sysThreadJoin(s_stream_tid, &r);
    s_stream_unjoined = false;
    g_stream_cancel = false;
}

void music_stop(void) {
    if (!s_started) return;
    crash_log("m5 music_stop");
    s_cmd = MCMD_STOP;
    s_run = false;
    g_stream_cancel = true;   // unblock a stream_open header wait
    u64 retval;
    // NOT the stream thread.  2026-09-24: after a network stall the user
    // switched track and then left; that thread was inside the track change's
    // blocking HTTP calls (session, playing report, stream open -- up to 5 s
    // each on a dead network) and this join froze the whole screen behind
    // them.  It only touches the PCM ring, its socket and HTTP, all safe to
    // let finish on their own: it sees s_run and ends, and music_start()
    // joins it before a new session reuses anything.
    sysThreadJoin(s_pump_tid, &retval);
    s_stream_unjoined = true;
    crash_log("m6 pump joined");
    audio_close();
    audio_set_paced(false);
    audio_set_source(NULL, NULL, NULL);   // hand the port back to the video path
    // Let a queued progress report go out, then retire the worker.  Bounded at
    // one second: leaving the music screen must not wait on a server that has
    // stopped answering.
    jellyfin_report_flush();
    s_started = false;
    s_active  = false;
    plog("music: stopped");
}

void music_toggle_pause(void) {
    s_paused = !s_paused;
    // Logged because the last hardware freeze happened within a second of a
    // pause and there was no way to tell, afterwards, whether the app stopped
    // because of the pause or merely while paused.
    {
        char b[64];
        snprintf(b, sizeof b, "music: %s at %llus",
                 s_paused ? "PAUSE" : "RESUME",
                 (unsigned long long)(elapsed_ticks() / 10000000ULL));
        plog(b);
    }
    // Push the state so the server UI flips too -- but hand it to the report
    // thread, because this runs on the RENDER LOOP, from the music screen's
    // input handler.  The blocking version froze every frame until the server
    // answered, which is a whole second of dead UI for a button press whose
    // own effect (the flag above) is instant.
    if (s_ui_pos < s_count)
        jellyfin_report_progress_async(s_queue[s_order[s_ui_pos]].id, s_session_id,
                                       elapsed_ticks(), s_paused);
}

void music_next(void) { s_cmd = MCMD_NEXT; }

// Rebuild the play order around whatever is playing right now: shuffle
// pulls the current track to position 0 and Fisher-Yates the rest behind
// it; un-shuffle restores library order with the position following the
// current track.  s_order[s_pos] keeps pointing at the playing track
// through either rewrite, so playback is never interrupted.
void music_set_shuffle(bool on) {
    if (s_count <= 1) { s_shuffle = on; return; }
    sysMutexLock(s_pcm_mtx, 0);
    const int dec_idx = s_order[s_pos];
    int cur_idx = s_order[s_ui_pos];
    if (on) {
        int n = 0;
        int rest[MUSIC_QUEUE_MAX];
        for (int i = 0; i < s_count; i++)
            if (i != cur_idx) rest[n++] = i;
        for (int i = n - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            int tmp = rest[i]; rest[i] = rest[j]; rest[j] = tmp;
        }
        s_order[0] = cur_idx;
        for (int i = 0; i < n; i++) s_order[i + 1] = rest[i];
    } else {
        for (int i = 0; i < s_count; i++) s_order[i] = i;
    }
    for (int i = 0; i < s_count; i++) {
        if (s_order[i] == cur_idx) s_ui_pos = i;
        if (s_order[i] == dec_idx) { s_pos = i; if (s_bnd_pending) s_bnd_pos = i; }
    }
    sysMutexUnlock(s_pcm_mtx);
    s_shuffle = on;
}

bool music_is_shuffle(void) { return s_shuffle; }

int music_current_pos(void) { return s_ui_pos; }

int music_track_at(int pos) {
    if (pos < 0 || pos >= s_count) return -1;
    return s_order[pos];
}

void music_prev(void) {
    if (music_elapsed_secs() > 3) {
        s_cmd_arg = 0;
        s_cmd     = MCMD_SEEK;      // restart the current track
    } else {
        s_cmd = MCMD_PREV;
    }
}

void music_seek(int delta_secs) {
    int target = (int)music_elapsed_secs() + delta_secs;
    if (target < 0) target = 0;
    if (s_duration > 0 && target > (int)s_duration - 1)
        target = (int)s_duration - 1;
    s_cmd_arg = target;
    s_cmd     = MCMD_SEEK;
}

void music_jump(int queue_pos) {
    s_cmd_arg = queue_pos;
    s_cmd     = MCMD_JUMP;
}

bool music_is_active(void)    { return s_started && s_active; }
bool music_is_paused(void)    { return s_paused; }
int  music_current_index(void){ return s_order[s_ui_pos]; }

u32 music_elapsed_secs(void) {
    return s_seek_base + (u32)(s_consumed / 48000ULL);
}

u32         music_duration_secs(void) { return s_duration; }
const char *music_source_info(void)   { return s_src_info; }

// -------------------------------------------------------
// Album track fetch
// -------------------------------------------------------

static int music_fetch_tracks_url(const char *url, MusicTrack *out, int max);

int music_fetch_album_tracks(const char *album_id, MusicTrack *out, int max) {
    char url[512];
    snprintf(url, sizeof(url),
        "%s/Users/%s/Items?ParentId=%s"
        "&IncludeItemTypes=Audio&Recursive=true"
        "&SortBy=ParentIndexNumber,IndexNumber,SortName&SortOrder=Ascending"
        "&Limit=%d&Fields=RunTimeTicks",
        g_server, g_userid, album_id,
        max < MUSIC_QUEUE_MAX ? max : MUSIC_QUEUE_MAX);
    return music_fetch_tracks_url(url, out, max);
}

// Playlists keep their curated order: no SortBy, direct children only.
int music_fetch_playlist_tracks(const char *playlist_id,
                                MusicTrack *out, int max) {
    char url[512];
    snprintf(url, sizeof(url),
        "%s/Users/%s/Items?ParentId=%s"
        "&IncludeItemTypes=Audio"
        "&Limit=%d&Fields=RunTimeTicks",
        g_server, g_userid, playlist_id,
        max < MUSIC_QUEUE_MAX ? max : MUSIC_QUEUE_MAX);
    return music_fetch_tracks_url(url, out, max);
}

static int music_fetch_tracks_url(const char *url, MusicTrack *out, int max) {
    int status = http_request(0, url, NULL, g_token,
                              responseBuffer, RESPONSE_SIZE);
    if (status != 200) return 0;

    const char *p = strstr(responseBuffer, "\"Items\":[");
    if (!p) return 0;
    p += 9;

    int count = 0;
    while (*p && count < max) {
        while (*p && *p != '{' && *p != ']') p++;
        if (!*p || *p == ']') break;
        const char *obj = p;
        int depth = 0; bool in_str = false, esc = false;
        while (*p) {
            char c = *p;
            if (esc) { esc = false; }
            else if (in_str) { if (c=='\\') esc=true; else if (c=='"') in_str=false; }
            else { if (c=='"') in_str=true; else if (c=='{') depth++; else if (c=='}') { if (--depth==0){p++;break;} } }
            p++;
        }
        int olen = (int)(p - obj);

        MusicTrack *t = &out[count];
        memset(t, 0, sizeof(*t));
        xmb_json_str_range(obj, olen, "Id",   t->id,   sizeof(t->id));
        xmb_json_str_range(obj, olen, "Name", t->name, sizeof(t->name));
        decode_unicode_escapes(t->name);
        if (!xmb_json_first_arr_str(obj, olen, "Artists",
                                    t->artist, sizeof(t->artist)))
            xmb_json_str_range(obj, olen, "AlbumArtist",
                               t->artist, sizeof(t->artist));
        decode_unicode_escapes(t->artist);
        t->track_num = xmb_json_int_range(obj, olen, "IndexNumber", 0);
        long long ticks = xmb_json_ll_range(obj, olen, "RunTimeTicks", 0);
        if (ticks > 0) t->duration_secs = (u32)(ticks / 10000000LL);
        xmb_json_str_range(obj, olen, "AlbumId",
                           t->art_id, sizeof(t->art_id));
        if (!t->art_id[0])
            strncpy(t->art_id, t->id, sizeof(t->art_id) - 1);
        if (t->id[0]) count++;
    }
    return count;
}
