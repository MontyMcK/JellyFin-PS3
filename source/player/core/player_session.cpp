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
#include "track_codec.h"
#include "ui.h"
#include "ui_visuals.h"
#include "rsxutil.h"
#include "jellyfin_api.h"

// -------------------------------------------------------
// Helper — wait for O with error message
// -------------------------------------------------------

void show_error(const char *line1, const char *line2) {
#if !BUILD_FOR_RPCS3
    drawHeader();
    drawText(40, 100, line1);
    if (line2 && line2[0]) drawText(40, 130, line2);
    drawText(40, 160, "O: back");
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
// -------------------------------------------------------
// Audio/subtitle params are omitted at -1 (server default audio / no
// subtitles).  Subtitles are burned in server-side (SubtitleMethod=Encode)
// since the PS3 client has no subtitle renderer.

void build_stream_url(char *url, int url_sz, const PlayerState *ps,
                      u64 start_ticks) {
    int audio_idx = player_audio_stream_idx(ps);
    int sub_idx   = player_sub_stream_idx(ps);
    // 1080p (Alpha): 1080p exceeds baseline/level-3.1, so ask for a High-profile
    // level-4.2 transcode at a higher ceiling bitrate.  Gated — OFF reproduces
    // the exact baseline/31/4Mbps query the 720p ship path sends.
    // Resolved through the quality row on the info screen; at its default
    // (Auto) this is exactly the hd-toggle pair above it used to be.  Note
    // req_w/req_h were resolved by the same call in player.cpp, so the URL
    // and the jitter buffer cannot disagree about the frame size.
    const bool  hd = hd1080_enabled();
    const char *profile;
    const char *level;
    unsigned    vbitrate;
    vquality_params(vquality_get(), hd, 0, 0, NULL, NULL,
                    &profile, &level, &vbitrate);
    // Surround 5.1 (Alpha): request an AC-3 5.1 transcode at the standard DVD
    // rate.  Gated — OFF reproduces the exact stereo MP3 query the ship path
    // sends.  This builder is reused by every seek (player_seek.cpp), so the
    // codec choice is stable across seeks by construction.
    const bool  surround = surround_enabled();
    const char *acodec   = surround ? "ac3"    : "mp3";
    unsigned    abitrate = surround ? 640000u  : 192000u;
    int         achans   = surround ? 6        : 2;
    // Surround "HD" mode: ask the server to STREAM-COPY the source's own HD
    // audio track instead of transcoding it.  Copy is the only way either of
    // these formats ever arrives — Jellyfin will not transcode TO them
    // (ffmpeg's dca encoder is experimental and its truehd encoder tops out
    // at 5.1) — and it is also the whole point:
    //   * a DTS-HD MA / DTS:X track copied intact still carries the 5.1 core
    //     this app decodes, at up to 1509 kbps;
    //   * a TrueHD / Atmos track copied intact decodes here LOSSLESSLY, to
    //     5.1 or 7.1.
    // Either way there is no audio transcode on the server at all.
    //
    // Only asked for when the SELECTED track really is one of those (its
    // label says so).  On any other track — including Dolby Digital Plus,
    // which nothing here decodes — the request is the plain AC-3 one, which
    // is why HD mode never plays worse than AC-3 mode.  Four knobs differ
    // from the AC-3 request and all of them matter:
    //   * AllowAudioStreamCopy=true — without it the server transcodes and
    //     the "dts"/"truehd" preference lands on an encoder we do not want.
    //   * no AudioBitrate — Jellyfin checks the requested audio bitrate
    //     before allowing a copy, and a 1509 kbps DTS core (let alone a
    //     multi-Mbps TrueHD stream) fails a 640 kbps ceiling, silently
    //     demoting us to a transcode.
    //   * MaxAudioChannels=8, not 6 — DTS-HD MA, DTS:X and TrueHD tracks are
    //     routinely 7.1, and a 6-channel ceiling would refuse to copy them.
    //     Copying them is right: TrueHD then plays as a real 7.1 program, and
    //     what this app decodes out of a DTS-HD track is its 5.1 core either
    //     way.
    //   * ac3 is listed FIRST even though the HD codec is the one being asked
    //     for.  Copy eligibility only asks whether the source codec appears
    //     in the list; the order decides what an actual transcode would
    //     encode to, and that must be ac3.
    const char *hd_codec = NULL;
    if (surround && surround_hd_preferred() && ps->cur_audio >= 0) {
        const char *label = ps->tracks.audio[ps->cur_audio].label;
        if      (track_label_is_dts(label))    hd_codec = "dts";
        else if (track_label_is_truehd(label)) hd_codec = "truehd";
    }
    char aparams[128];
    if (hd_codec) {
        snprintf(aparams, sizeof(aparams),
                 "&AudioCodec=ac3,%s,mp3&AudioSampleRate=48000"
                 "&MaxAudioChannels=8", hd_codec);
    } else {
        snprintf(aparams, sizeof(aparams),
                 "&AudioCodec=%s&AudioBitrate=%u&AudioSampleRate=48000"
                 "&MaxAudioChannels=%d", acodec, abitrate, achans);
    }
    const char *copy_audio = hd_codec ? "true" : "false";
    const JFMediaSource *source = player_current_source(ps);
    const char *source_id = (source && source->id[0]) ? source->id : ps->item->id;
    char encoded_source[288];
    url_encode_query(source_id, encoded_source, sizeof(encoded_source));
    // Video request.  vbitrate == 0 is DIRECT PLAY (the "Original" quality
    // setting): ask the server to copy the source video through untouched.
    //
    // The ceiling has to be ABSENT, not just large.  Jellyfin checks the
    // requested bitrate before it will allow a copy, so a 10 Mbps ask against
    // a 30 Mbps remux does not clamp the copy -- it refuses it and re-encodes,
    // silently. That is the same trap the HD audio path documents above for
    // AudioBitrate, and it fails the same quiet way.
    //
    // MaxWidth/MaxHeight and MaxFramerate stay in both cases and act as the
    // safety gate: a 4K or 60 fps source exceeds them, so the server scales it
    // down rather than copying something this console cannot decode.
    char vparams[96];
    if (vbitrate == 0)
        snprintf(vparams, sizeof(vparams), "&AllowVideoStreamCopy=true");
    else
        snprintf(vparams, sizeof(vparams),
                 "&VideoBitrate=%u&AllowVideoStreamCopy=false", vbitrate);

    int n = snprintf(url, url_sz,
        "%s/Videos/%s/stream.ts"
        "?VideoCodec=h264"
        "&Profile=%s"
        "&Level=%s"
        "&MaxWidth=%u&MaxHeight=%u"
        "%s"
        "%s"
        "&MaxFramerate=30"
        "&AllowAudioStreamCopy=%s"
        "&DeviceId=%s&Static=false"
        "&MediaSourceId=%s"
        "&StartTimeTicks=%llu",
        g_server, ps->item->id, profile, level, ps->req_w, ps->req_h, vparams,
        aparams, copy_audio,
        jf_device_id(), encoded_source, (unsigned long long)start_ticks);
    if (source && source->live_stream_id[0] && n > 0 && n < url_sz) {
        char encoded_live[288];
        url_encode_query(source->live_stream_id, encoded_live, sizeof(encoded_live));
        n += snprintf(url + n, url_sz - n, "&LiveStreamId=%s", encoded_live);
    }
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
