// Session helpers — error screen, URL building, jitter-buffer prefill.

#include <stdio.h>
#include <string.h>

#include <ppu-types.h>
#include <sysutil/sysutil.h>

#include "player_internal.h"
#include "stream.h"
#include "video.h"
#include "plog.h"
#include "ui.h"
#include "ui_visuals.h"
#include "rsxutil.h"
#include "jellyfin_api.h"

// -------------------------------------------------------
// Helper — wait for O with error message
// -------------------------------------------------------

void show_error(const char *line1, const char *line2) {
    drawHeader();
    drawText(40, 100, line1);
    if (line2 && line2[0]) drawText(40, 130, line2);
    drawText(40, 160, "O: back");
    flip();
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

// -------------------------------------------------------
// Stream URL builder — used for the initial open and for every seek.
// start_ticks is in Jellyfin's 100-ns units (seconds * 10,000,000).
// The query string is otherwise identical so the server keeps the same
// transcode session and we only change the start offset.
// -------------------------------------------------------
// Audio/subtitle params are omitted at -1 (server default audio / no
// subtitles).  Subtitles are burned in server-side (SubtitleMethod=Encode)
// since the PS3 client has no subtitle renderer.

void build_stream_url(char *url, int url_sz, const PlayerState *ps,
                      u64 start_ticks) {
    int audio_idx = player_audio_stream_idx(ps);
    int sub_idx   = player_sub_stream_idx(ps);
    int n = snprintf(url, url_sz,
        "%s/Videos/%s/stream.ts"
        "?VideoCodec=h264"
        "&Profile=baseline"
        "&Level=31"
        "&MaxWidth=%u&MaxHeight=%u"
        "&VideoBitrate=4000000"
        "&AudioCodec=mp3&AudioBitrate=192000&AudioSampleRate=48000"
        "&MaxAudioChannels=2"
        "&MaxFramerate=30"
        "&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false"
        "&DeviceId=ps3&Static=false"
        "&MediaSourceId=%s"
        "&StartTimeTicks=%llu",
        g_server, ps->item->id, ps->req_w, ps->req_h, ps->item->id,
        (unsigned long long)start_ticks);
    if (audio_idx >= 0 && n > 0 && n < url_sz)
        n += snprintf(url + n, url_sz - n, "&AudioStreamIndex=%d", audio_idx);
    if (sub_idx >= 0 && n > 0 && n < url_sz)
        n += snprintf(url + n, url_sz - n,
                      "&SubtitleStreamIndex=%d&SubtitleMethod=Encode", sub_idx);
    if (ps->session_id[0] && n > 0 && n < url_sz) {
        snprintf(url + n, url_sz - n, "&PlaySessionId=%s", ps->session_id);
    }
}

// -------------------------------------------------------
// Pre-fill: decode JBUF_PREFILL frames before display starts/resumes
// -------------------------------------------------------

void player_prefill(PlayerState *ps, bool fatal_on_eof, int guard_max) {
    u8   ts_pkt[TS_PACKET_SIZE];
    bool first_pkt = true;
    int  guard     = 0;
    while (jbuf_count() < JBUF_PREFILL && running && !s_vdec_error &&
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
