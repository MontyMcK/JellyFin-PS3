// Session helpers — error screen, URL building, jitter-buffer prefill.

#include <stdio.h>
#include <string.h>

#include <ppu-types.h>
#include <sysutil/sysutil.h>

#include "../../build_config.h"   // relative: source/ is not on the -I path
#include "player_internal.h"
#include "stream.h"
#include "video.h"
#include "plog.h"
#include "hd1080.h"
#include "vquality.h"
#include "surround.h"
#include "audio_bitstream.h"
#include "track_codec.h"
#include "ui.h"
#include "ui_visuals.h"
#include "rsxutil.h"
#include "jellyfin_api.h"
#include "jf_paths.h"
#include "stream_request.h"
#include "directstream.h"

// jellyfin_abudget.txt: 0 asks for the quality step as the VIDEO bitrate;
// missing or anything else budgets the audio inside the step.  Read per
// request (a seek rebuilds the URL), which is rare.
static bool stream_budget_enabled(void) {
    FILE *f = fopen(jf_data_path("jellyfin_abudget.txt"), "r");
    if (!f) return true;
    int v = 1;
    if (fscanf(f, "%d", &v) != 1) v = 1;
    fclose(f);
    return v != 0;
}

// -------------------------------------------------------
// Helper — wait for O with error message
// -------------------------------------------------------
void show_error(const char *line1, const char *line2) {
#if !BUILD_FOR_RPCS3
    drawHeader();
    drawText(40, 100, tr(line1));
    if (line2 && line2[0]) drawText(40, 130, tr(line2));
    drawText(40, 160, TR("O: back"));
    flip();
#else
    // Emulator: the bitmap-font (NV3089) glyph blits dead-lock the RSX FIFO in
    // the fragile post-failure state (Dead FIFO ... nv3089 decode_transfer),
    // taking the whole emulator down instead of showing this message.  So drop
    // the text and just hold a drained frame; the reason is in player_log.txt.
    // Mirrors player_status_screen()'s gating in player.cpp.
    (void)line1; (void)line2;
    flip();
#endif
    init_btns();
    while (running) {
        sysUtilCheckCallback();
        poll_buttons();
        if (BTN_PRESSED(circle)) return;
    }
}

// plog() truncates every line at 127 chars, so a 400+ char stream URL is cut
// off well before StartTimeTicks/PlaySessionId.  Log it in ~100-char chunks so
// the full query string is visible on the wire.
void plog_url(const char *tag, const char *url) {
    int len = (int)strlen(url);
    char buf[128];
    int part = 0;
    for (int off = 0; off < len; off += 100, part++) {
        snprintf(buf, sizeof(buf), "%s[%d]: %.100s", tag, part, url + off);
        plog(buf);
    }
}

// -------------------------------------------------------
// Track selection — Jellyfin MediaStream indices
// -------------------------------------------------------

int player_audio_stream_idx(const PlayerState *ps) {
    return (ps->cur_audio >= 0) ? ps->tracks.audio[ps->cur_audio].index : -1;
}

int player_sub_stream_idx(const PlayerState *ps) {
    return (ps->cur_sub >= 0) ? ps->tracks.subs[ps->cur_sub].index : -1;
}

const JFMediaSource *player_current_source(const PlayerState *ps) {
    return ps->source.id[0] ? &ps->source : NULL;
}

// -------------------------------------------------------
// Stream URL builder — used for the initial open and for every seek.
// start_ticks is in Jellyfin's 100-ns units (seconds * 10,000,000).
// The query string is otherwise identical so the server keeps the same
// transcode session and we only change the start offset.
//
// The decision itself (quality ladder, copy vs transcode, HD audio copy,
// stream indices) lives in stream/stream_request.cpp, shared with offline
// downloads so the two can never choose differently.  This is the player's
// adapter: its selection in, its session and offset on top.
// -------------------------------------------------------

void stream_prefs_current(StreamPrefs *out) {
    out->quality     = vquality_get();
    out->hd1080      = hd1080_enabled();
    out->surround    = surround_enabled();
    out->surround_hd = surround_hd_preferred();
    out->display_w   = display_width;
    out->display_h   = display_height;
    out->budget      = stream_budget_enabled();
    out->passthrough = audio_passthrough_wanted();
    out->no_video_copy = !directstream_enabled();
}

void build_stream_url(char *url, int url_sz, const PlayerState *ps,
                      u64 start_ticks) {
    StreamPrefs prefs;
    stream_prefs_current(&prefs);
    StreamSelection sel = { ps->item->id, player_current_source(ps),
                            &ps->tracks, ps->cur_audio, ps->cur_sub,
                            ps->sub_is_text || ps->sub_is_pgs, ps->live };
    StreamRequest rq;
    stream_request_resolve(&prefs, &sel, &rq);
    // The frame ceiling the jitter buffer was allocated for at open.  A seek
    // keeps it, so it can never ask for a different frame size.
    rq.max_w = ps->req_w;
    rq.max_h = ps->req_h;
    if (rq.budgeted) {
        char lb[128];
        snprintf(lb, sizeof(lb),
                 "stream budget: step=%u video=%u audio=%u (%s, reported %u)",
                 rq.vbitrate, rq.vreq, rq.audio_cost,
                 rq.hd_codec ? rq.hd_codec : rq.acodec, rq.audio_reported);
        plog(lb);
    }
    stream_url_build(url, url_sz, &rq, g_server, jf_device_id(),
                     ps->session_id, (unsigned long long)start_ticks);
}

// -------------------------------------------------------
// Pre-fill: decode JBUF_PREFILL frames before display starts/resumes
// -------------------------------------------------------

void player_prefill(PlayerState *ps, bool fatal_on_eof, int guard_max) {
    u8   ts_pkt[TS_PACKET_SIZE];
    bool first_pkt = true;
    int  guard     = 0;
    while (jbuf_count() < jbuf_prefill_target() && running && !s_vdec_error &&
           guard < guard_max) {
        sysUtilCheckCallback();
        int rd = stream_read(ps->sock, ts_pkt, TS_PACKET_SIZE);
        if (rd < 0) {
            if (fatal_on_eof) {
                plog("playing=0 reason=net_error");
                ps->playing = false;
            } else {
                plog("seek: prefill eof");
            }
            break;
        }
        if (rd > 0) {
            if (first_pkt && fatal_on_eof) {
                char buf[56];
                snprintf(buf, sizeof(buf),
                         "show_player: first pkt byte=0x%02x", ts_pkt[0]);
                plog(buf);
                first_pkt = false;
            }
            video_feed_ts(ts_pkt);
        }
        guard++;
    }
}
