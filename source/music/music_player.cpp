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
#include "minimp3.h"
#include "audio.h"
#include "stream.h"
#include "plog.h"
#include "timing.h"
#include "jellyfin_api.h"
#include "ui_internal.h"        // xmb_json_* helpers for the track fetch

extern u32 running;

// ---- PCM ring (interleaved float L/R pairs, power of two) ----
#define MPCM_CAP 32768          // ~683 ms at 48 kHz
static float        s_ring[MPCM_CAP * 2];
static int          s_wr = 0, s_rd = 0;
static volatile int s_n  = 0;
static sys_mutex_t  s_pcm_mtx;
static bool         s_pcm_mtx_ok = false;

// ---- queue + engine state ----
// Playback walks s_order (a permutation of 0..count-1) by position; shuffle
// just rewrites the permutation, so the queue overlay and Up Next always
// reflect the true play order.  s_order[s_pos] is the original track index.
static MusicTrack   s_queue[MUSIC_QUEUE_MAX];
static int          s_order[MUSIC_QUEUE_MAX];
static int          s_count  = 0;
static volatile int s_pos    = 0;
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

static sys_ppu_thread_t s_stream_tid = 0;
static sys_ppu_thread_t s_pump_tid   = 0;

// -------------------------------------------------------
// PCM ring + audio source callbacks
// -------------------------------------------------------

static void mring_flush(void) {
    sysMutexLock(s_pcm_mtx, 0);
    s_wr = s_rd = 0;
    s_n  = 0;
    sysMutexUnlock(s_pcm_mtx);
}

static int mring_space(void) { return MPCM_CAP - s_n; }

static void mring_push(const float *lr, int n_pairs) {
    sysMutexLock(s_pcm_mtx, 0);
    for (int i = 0; i < n_pairs; i++) {
        if (s_n >= MPCM_CAP) break;
        s_ring[s_wr * 2    ] = lr[i * 2    ];
        s_ring[s_wr * 2 + 1] = lr[i * 2 + 1];
        s_wr = (s_wr + 1) & (MPCM_CAP - 1);
        s_n++;
    }
    sysMutexUnlock(s_pcm_mtx);
}

static int music_pcm_avail(void) { return s_n; }

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
    s_consumed += (u64)got;
    sysMutexUnlock(s_pcm_mtx);
    // Visualizer tap lives here, not at decode time: these samples hit the
    // hardware DMA ring (~40 ms of latency) now, while the decode cursor can
    // run ~700 ms ahead through the PCM ring — bars must move with what's
    // audible, not with what's buffered.
    music_viz_push(buf, got);
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
static void track_session_setup(const MusicTrack *t) {
    s_session_id[0] = '\0';
    s_src_info[0]   = '\0';
    s_duration      = t->duration_secs;

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
        "&DeviceId=ps3"
        "&StartTimeTicks=%llu",
        g_server, item_id, (unsigned long long)start_secs * 10000000ULL);
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
static int play_one_track(u32 start_secs) {
    const MusicTrack *t = &s_queue[s_order[s_pos]];

    mring_flush();
    music_viz_reset();
    mp3dec_init(&s_dec);
    s_consumed  = 0;
    s_seek_base = start_secs;

    char buf[160];
    snprintf(buf, sizeof(buf), "music: track %d/%d start=%us id=%.16s",
             s_pos + 1, s_count, start_secs, t->id);
    plog(buf);

    track_session_setup(t);
    jellyfin_report_playing(t->id, s_session_id,
                            (u64)start_secs * 10000000ULL);

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
    static u8 mp3[65536];
    int  buf_pos = 0, buf_len = 0;
    bool eof = false;
    u64  last_prog_us = timing_get_us();
    int  ret = MCMD_NONE;

    while (running && s_run) {
        int c = take_cmd();
        if (c != MCMD_NONE) { ret = c; break; }

        // ~10 s progress heartbeat keeps the server's session view honest.
        u64 now = timing_get_us();
        if (now - last_prog_us >= 10000000ULL) {
            last_prog_us = now;
            jellyfin_report_progress(t->id, s_session_id, elapsed_ticks(),
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
                if (buf_len - buf_pos < 8192) continue;    // buffer more first
            }
        }

        // Decode while there's data and the ring has room for a frame.
        bool progressed = false;
        while (buf_pos < buf_len) {
            if (mring_space() < 1300) break;   // ring nearly full
            mp3dec_frame_info_t info;
            short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
            int samples = mp3dec_decode_frame(&s_dec, mp3 + buf_pos,
                                              buf_len - buf_pos, pcm, &info);
            if (info.frame_bytes <= 0) break;  // needs more bytes
            buf_pos += info.frame_bytes;
            progressed = true;
            if (samples > 0) {
                static float out[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
                int pairs = frame_to_48k(pcm, samples, info.channels, info.hz,
                                         out, MINIMP3_MAX_SAMPLES_PER_FRAME);
                mring_push(out, pairs);
            }
        }

        // Slide leftovers down once the consumed prefix gets large.
        if (buf_pos > (int)sizeof(mp3) / 2) {
            memmove(mp3, mp3 + buf_pos, buf_len - buf_pos);
            buf_len -= buf_pos;
            buf_pos  = 0;
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

    netClose(sock);
    jellyfin_report_stopped(t->id, s_session_id, elapsed_ticks());
    jellyfin_stop_transcode(s_session_id);
    return ret;
}

static void music_stream_thread(void *arg) {
    (void)arg;
    u32 start_secs = 0;
    while (running && s_run) {
        int end = play_one_track(start_secs);
        start_secs = 0;
        if (!s_run || end == MCMD_STOP) break;
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
    plog("music: stream thread exit");
    sysThreadExit(0);
}

// -------------------------------------------------------
// Pump thread — services the audio port DMA from the ring
// -------------------------------------------------------

static void music_pump_thread(void *arg) {
    (void)arg;
    while (running && s_run) {
        if (s_paused || !s_active || !audio_write_pcm())
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
    s_shuffle   = false;
    srand((unsigned)timing_get_us());
    s_cmd       = MCMD_NONE;
    s_paused    = false;
    s_seek_base = 0;
    s_consumed  = 0;
    s_duration  = 0;
    s_src_info[0] = '\0';
    mring_flush();
    music_viz_reset();

    audio_set_source(music_pcm_avail, music_read_pcm);
    audio_open();

    s_run     = true;
    s_active  = true;
    s_started = true;
    sysThreadCreate(&s_pump_tid, music_pump_thread, NULL,
                    700, 0x8000, THREAD_JOINABLE, (char*)"jf_mpump");
    sysThreadCreate(&s_stream_tid, music_stream_thread, NULL,
                    850, 0x20000, THREAD_JOINABLE, (char*)"jf_music");
    return true;
}

void music_stop(void) {
    if (!s_started) return;
    s_cmd = MCMD_STOP;
    s_run = false;
    g_stream_cancel = true;   // unblock a stream_open header wait
    u64 retval;
    sysThreadJoin(s_stream_tid, &retval);
    sysThreadJoin(s_pump_tid, &retval);
    g_stream_cancel = false;
    audio_close();
    audio_set_source(NULL, NULL);   // hand the port back to the video path
    s_started = false;
    s_active  = false;
    plog("music: stopped");
}

void music_toggle_pause(void) {
    s_paused = !s_paused;
    // Push the state immediately so the server UI flips too.
    if (s_pos < s_count)
        jellyfin_report_progress(s_queue[s_order[s_pos]].id, s_session_id,
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
    int cur_idx = s_order[s_pos];
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
        s_pos = 0;
    } else {
        for (int i = 0; i < s_count; i++) s_order[i] = i;
        s_pos = cur_idx;
    }
    s_shuffle = on;
}

bool music_is_shuffle(void) { return s_shuffle; }

int music_current_pos(void) { return s_pos; }

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
int  music_current_index(void){ return s_order[s_pos]; }

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
