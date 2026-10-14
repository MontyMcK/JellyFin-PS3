// Offline downloads -- from a Jellyfin item to a download request.
// See dl_request.h.

#include "dl_request.h"

#include <stdio.h>
#include <string.h>

static void copy(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src ? src : "");
}

// Direct play of a disc remux: video up to 40 Mbit/s plus the audio track.
#define DL_DISC_MAX_BPS 50000000ull
// An HD audio copy when the track's own rate is not known.
#define DL_HD_AUDIO_BPS  4500000ull

bool dl_estimate_bytes(const StreamRequest *d, uint32_t runtime_secs,
                       uint64_t source_size_bytes, uint64_t *out) {
    uint64_t base;
    if (d->vreq == 0) {
        if (source_size_bytes) base = source_size_bytes;
        else if (runtime_secs) base = DL_DISC_MAX_BPS / 8 * runtime_secs;
        else return false;
    } else {
        if (!runtime_secs) return false;
        const uint64_t audio = d->hd_codec
            ? (d->audio_cost ? d->audio_cost : DL_HD_AUDIO_BPS)
            : d->abitrate;
        base = ((uint64_t)d->vreq + audio) / 8 * runtime_secs;
    }
    *out = base + base / 100 * 15;
    return true;
}

bool dl_request_build(const DlRequestInput *in, DlRequest *out) {
    if (!in || !in->item || !in->source || !in->prefs || !in->server ||
        !in->server[0] || !in->device_id || !dl_id_valid(in->item->id) ||
        !in->item->name[0])
        return false;

    static DlRequest r;   // ~5 KB; the item page calls this on its own thread
    memset(&r, 0, sizeof(r));

    // ---- the stream: the player's selection rule and the player's decision
    const JFTracks *tracks = &in->source->tracks;
    StreamSelection sel = {};     // every field set or zero: nothing here is live
    sel.item_id = in->item->id;
    sel.source  = in->source;
    sel.tracks  = tracks;
    stream_select_initial(tracks, true, &sel.cur_audio, &sel.cur_sub);
    stream_request_resolve(in->prefs, &sel, &r.decision);
    // StartTimeTicks=0: the whole title.  No PlaySessionId: no session.
    int n = stream_url_build(r.url, sizeof(r.url), &r.decision, in->server,
                             in->device_id, NULL, 0ULL);
    if (n <= 0 || n >= (int)sizeof(r.url)) return false;

    // ---- metadata: identify, show and play it with the server gone
    DlMeta *m = &r.meta;
    dl_meta_init(m);
    copy(m->id,    sizeof(m->id),    in->item->id);
    copy(m->type,  sizeof(m->type),  in->item->type);
    copy(m->title, sizeof(m->title), in->item->name);
    const JFItemIdentity *idn = in->detail ? &in->detail->identity : NULL;
    if (idn) {
        copy(m->series,    sizeof(m->series),    idn->series_name);
        copy(m->series_id, sizeof(m->series_id), idn->series_id);
        m->season  = idn->season;
        m->episode = idn->episode;
        m->year    = idn->year;
        m->runtime_secs = idn->runtime_secs;
    }
    if (m->runtime_secs == 0) m->runtime_secs = in->source->runtime_secs;
    if (in->detail) {
        copy(m->overview,   sizeof(m->overview),   in->detail->overview);
        copy(m->video_info, sizeof(m->video_info), in->detail->video_info);
        copy(m->audio_info, sizeof(m->audio_info), in->detail->audio_info);
    }
    // What the FILE will be -- from the decision, not from the source.
    const StreamRequest *d = &r.decision;
    copy(m->container,   sizeof(m->container),   "ts");
    copy(m->video_codec, sizeof(m->video_codec), "h264");
    copy(m->audio_codec, sizeof(m->audio_codec), d->hd_codec ? d->hd_codec : d->acodec);
    m->width  = (int)d->max_w;          // a ceiling: the server may send less
    m->height = (int)d->max_h;
    m->audio_channels = d->hd_codec ? 8 : d->achans;   // HD copy: up to 7.1
    m->video_bitrate  = d->vbitrate;
    copy(m->media_source_id, sizeof(m->media_source_id), d->source_id);
    m->audio_stream_index = d->audio_idx;
    copy(m->quality, sizeof(m->quality), vquality_label(in->prefs->quality));
    // The track actually downloaded beats the item's default description.
    if (sel.cur_audio >= 0 && sel.cur_audio < tracks->n_audio)
        copy(m->audio_info, sizeof(m->audio_info), tracks->audio[sel.cur_audio].label);
    // poster/backdrop stay "" until the files really exist (dl_manager).

    // ---- artwork: the item's own Primary; its own Backdrop, else the
    // series' one an episode inherits.  Aspect kept (max*, not fill*).
    snprintf(r.poster_url, sizeof(r.poster_url),
             "%s/Items/%s/Images/Primary?maxWidth=%d&maxHeight=%d&quality=85&format=Jpeg",
             in->server, in->item->id, DL_POSTER_MAX_W, DL_POSTER_MAX_H);
    const char *bd = NULL;
    if (idn && idn->has_backdrop)              bd = in->item->id;
    else if (idn && idn->parent_backdrop_id[0]) bd = idn->parent_backdrop_id;
    if (bd)
        snprintf(r.backdrop_url, sizeof(r.backdrop_url),
                 "%s/Items/%s/Images/Backdrop?maxWidth=%d&quality=80&format=Jpeg",
                 in->server, bd, DL_BACKDROP_MAX_W);

    r.size_known = dl_estimate_bytes(d, m->runtime_secs, in->source->size_bytes,
                                     &r.size_estimate);
    r.extras.estimate_bytes = r.size_known ? r.size_estimate : 0;

    r.extras.container    = "ts";
    r.extras.expect_secs  = m->runtime_secs;
    r.extras.poster_url   = r.poster_url;
    r.extras.backdrop_url = r.backdrop_url[0] ? r.backdrop_url : NULL;

    *out = r;
    out->extras.poster_url   = out->poster_url;
    out->extras.backdrop_url = out->backdrop_url[0] ? out->backdrop_url : NULL;
    return true;
}
