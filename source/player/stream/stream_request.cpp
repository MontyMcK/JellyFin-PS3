// Stream request -- see stream_request.h.
//
// The one place a stream.ts request is decided.  The player and the offline
// downloader both call it, so a download cannot choose a different video
// codec, quality, audio track or copy/transcode mode than Play with the same
// selection.  tests/test_stream_request.c pins the URLs it produces.

#include "stream_request.h"
#include "track_codec.h"
#include "../core/stream_budget.h"

#include <stdio.h>
#include <string.h>

void stream_select_initial(const JFTracks *tracks, bool have_tracks,
                           int *cur_audio, int *cur_sub) {
    *cur_audio = (have_tracks && tracks && tracks->n_audio > 0)
                     ? tracks->default_audio : -1;
    *cur_sub = -1;               // subtitles start off
}

void stream_request_resolve(const StreamPrefs *prefs,
                            const StreamSelection *sel, StreamRequest *rq) {
    memset(rq, 0, sizeof(*rq));

    // Frame size, profile/level and bitrate step all come from the quality
    // ladder.  The display clamp only moves the sub-1080p steps' frame size.
    vquality_params(prefs->quality, prefs->hd1080,
                    prefs->display_w, prefs->display_h,
                    &rq->max_w, &rq->max_h,
                    &rq->profile, &rq->level, &rq->vbitrate);

    // A live channel is transcoded whatever the quality step: its source is a
    // tuner or an IPTV stream the console cannot be sent as it is.  With no
    // step chosen it gets a fixed ceiling rather than none.
    rq->live = sel->live;
    rq->no_video_copy = prefs->no_video_copy;
    if (rq->live && rq->vbitrate == 0) rq->vbitrate = STREAM_LIVE_VIDEO_BPS;

    const JFTracks *t = sel->tracks;
    const bool have_audio = t && sel->cur_audio >= 0 && sel->cur_audio < t->n_audio;
    const bool have_sub   = t && sel->cur_sub   >= 0 && sel->cur_sub   < t->n_subs;
    rq->audio_idx = have_audio ? t->audio[sel->cur_audio].index : -1;

    // Only a subtitle this app cannot draw itself goes to the server to be
    // burned in.  Burn-in forces a full video transcode, which gives up the
    // stream-copy path and with it a copied lossless audio track, so a track
    // drawn on the console (text, PGS) must not appear in the request.
    rq->sub_idx = (have_sub && !sel->sub_on_console) ? t->subs[sel->cur_sub].index : -1;

    // Stereo asks for MP3.  Any surround mode, and Dolby Digital output, asks
    // for AC-3 5.1 at the DVD rate; the request is stable across seeks because
    // every seek rebuilds it from the same settings.
    const bool ac3 = prefs->surround || prefs->passthrough;
    rq->acodec   = ac3 ? "ac3"   : "mp3";
    rq->abitrate = ac3 ? 640000u : 192000u;
    rq->achans   = ac3 ? 6       : 2;

    // 5.1/7.1 prefers the source's own HD audio track, stream-copied by the
    // server: Jellyfin cannot transcode TO DTS or TrueHD, and a copy leaves
    // the lossless track untouched.  Only when the SELECTED track's label
    // names one of those; any other track, Dolby Digital Plus included, takes
    // the plain AC-3 request.
    // Dolby Digital output never takes the HD copy: the receiver decodes
    // AC-3 and nothing else.  A track that already is Dolby Digital is copied,
    // so the receiver gets the source frames; any other track takes the AC-3
    // transcode above.  Never a TrueHD or DTS copy in this mode.
    rq->hd_codec = NULL;
    rq->ac3_copy = prefs->passthrough && have_audio &&
                   track_label_is_ac3(t->audio[sel->cur_audio].label);
    if (prefs->surround && prefs->surround_hd && have_audio && !prefs->passthrough) {
        const char *label = t->audio[sel->cur_audio].label;
        if      (track_label_is_dts(label))    rq->hd_codec = "dts";
        else if (track_label_is_truehd(label)) rq->hd_codec = "truehd";
    }

    // The quality step is the whole stream: the audio that rides with it and
    // the TS framing come out of the video ceiling.  Direct play (step 0)
    // keeps no ceiling at all.
    rq->vreq = rq->vbitrate;
    if (rq->vbitrate != 0 && prefs->budget) {
        int      kind     = STREAM_AUDIO_TRANSCODE;
        unsigned reported = rq->abitrate;
        if (rq->hd_codec) {
            kind = strcmp(rq->hd_codec, "truehd") == 0 ? STREAM_AUDIO_COPY_TRUEHD
                                                       : STREAM_AUDIO_COPY_DTS;
            reported = t->audio[sel->cur_audio].bitrate;
        }
        if (rq->ac3_copy && t->audio[sel->cur_audio].bitrate)
            reported = t->audio[sel->cur_audio].bitrate;
        rq->audio_reported = reported;
        rq->audio_cost     = stream_audio_cost(kind, reported);
        rq->vreq           = stream_video_budget(rq->vbitrate, rq->audio_cost);
        rq->budgeted       = true;
    }

    const JFMediaSource *src = (sel->source && sel->source->id[0]) ? sel->source : NULL;
    snprintf(rq->item_id, sizeof(rq->item_id), "%s", sel->item_id ? sel->item_id : "");
    snprintf(rq->source_id, sizeof(rq->source_id), "%s",
             src ? src->id : rq->item_id);
    snprintf(rq->live_stream_id, sizeof(rq->live_stream_id), "%s",
             src ? src->live_stream_id : "");
}

int stream_url_build(char *url, int url_sz, const StreamRequest *rq,
                     const char *server, const char *device_id,
                     const char *play_session_id,
                     unsigned long long start_ticks) {
    // An HD-audio copy differs from the AC-3 request in four ways, all of
    // which matter:
    //   * AllowAudioStreamCopy=true, or the server transcodes and the
    //     "dts"/"truehd" preference lands on an encoder it cannot use.
    //   * No AudioBitrate: Jellyfin checks the requested audio bitrate before
    //     allowing a copy, and a ceiling below the track's rate silently
    //     demotes the copy to a transcode.
    //   * MaxAudioChannels=8, not 6: DTS-HD MA and TrueHD tracks are often
    //     7.1 and a 6-channel ceiling would refuse to copy them.
    //   * ac3 is listed first: copy eligibility only asks whether the source
    //     codec is in the list, while the order decides what an actual
    //     transcode encodes to, and that must be ac3.
    char aparams[128];
    if (rq->ac3_copy) {
        // No AudioBitrate: a ceiling below the track's own rate demotes the
        // copy to a transcode.
        snprintf(aparams, sizeof(aparams),
                 "&AudioCodec=ac3&AudioSampleRate=48000&MaxAudioChannels=6");
    } else if (rq->hd_codec) {
        snprintf(aparams, sizeof(aparams),
                 "&AudioCodec=ac3,%s,mp3&AudioSampleRate=48000"
                 "&MaxAudioChannels=8", rq->hd_codec);
    } else {
        snprintf(aparams, sizeof(aparams),
                 "&AudioCodec=%s&AudioBitrate=%u&AudioSampleRate=48000"
                 "&MaxAudioChannels=%d", rq->acodec, rq->abitrate, rq->achans);
    }
    const char *copy_audio = (rq->hd_codec || rq->ac3_copy) ? "true" : "false";
    char encoded_source[288];
    url_encode_query(rq->source_id, encoded_source, sizeof(encoded_source));

    // A video ceiling of 0 is direct play and must be ABSENT from the URL, not
    // large: Jellyfin checks the requested bitrate before allowing a copy, so
    // any ceiling below the source demotes it to a transcode.  A ceiling is a
    // ceiling on the COPY: a source at or under it is copied untouched, one
    // over it is transcoded down to it.  MaxWidth/MaxHeight and MaxFramerate
    // stay in both cases as the gate that keeps a 4K or 60 fps source from
    // being copied to a console that cannot decode it.
    // Direct Stream = Off forbids the copy: a copied source keeps its own
    // quirks (interlacing, bitrate spikes, odd timestamps) all the way to the
    // PS3's decoder.
    const char *copy_video = rq->no_video_copy ? "false" : "true";
    char vparams[96];
    if (rq->live)
        snprintf(vparams, sizeof(vparams),
                 "&VideoBitrate=%u&AllowVideoStreamCopy=false", rq->vreq);
    else if (rq->vreq == 0)
        snprintf(vparams, sizeof(vparams), "&AllowVideoStreamCopy=%s", copy_video);
    else
        snprintf(vparams, sizeof(vparams),
                 "&VideoBitrate=%u&AllowVideoStreamCopy=%s", rq->vreq, copy_video);

    // The version's own id goes in the path: a source plugin keeps every
    // version as an item of its own, and asking for another item's path with
    // MediaSourceId=<version> answers 404.  For an ordinary item the two ids
    // are the same.  A live channel's path is the channel; its source id (the
    // opened stream) goes in MediaSourceId only.
    char encoded_path[288];
    if (rq->live) url_encode_query(rq->item_id, encoded_path, sizeof(encoded_path));
    else          snprintf(encoded_path, sizeof(encoded_path), "%s", encoded_source);
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
        server, encoded_path, rq->profile, rq->level, rq->max_w, rq->max_h,
        vparams, aparams, copy_audio,
        device_id, encoded_source, start_ticks);
    if (rq->live_stream_id[0] && n > 0 && n < url_sz) {
        char encoded_live[288];
        url_encode_query(rq->live_stream_id, encoded_live, sizeof(encoded_live));
        n += snprintf(url + n, url_sz - n, "&LiveStreamId=%s", encoded_live);
    }
    if (rq->audio_idx >= 0 && n > 0 && n < url_sz)
        n += snprintf(url + n, url_sz - n, "&AudioStreamIndex=%d", rq->audio_idx);
    if (rq->sub_idx >= 0 && n > 0 && n < url_sz)
        n += snprintf(url + n, url_sz - n,
                      "&SubtitleStreamIndex=%d&SubtitleMethod=Encode", rq->sub_idx);
    if (play_session_id && play_session_id[0] && n > 0 && n < url_sz)
        n += snprintf(url + n, url_sz - n, "&PlaySessionId=%s", play_session_id);
    return n;
}
