// Local playback: a downloaded media.ts, or a video file from a drive, through the existing player.
//
// Everything that decides something is portable and host-tested: which
// items are playable and how to set the player up (offline/dl_library.cpp),
// what a file is and which track to start with (local/local_probe.cpp), where
// to enter a transport stream for a start or a seek
// (player/stream/stream_local.cpp), and how a Matroska file becomes the
// transport stream the player reads (video/mkv_ts.cpp).  This file is the
// console glue: a read-at over lfs for the probes, the reopen, and the entry
// points.

#include <stdio.h>
#include <string.h>

#include <ppu-types.h>

#include "i18n.h"
#include "player.h"
#include "player_internal.h"
#include "stream.h"
#include "stream_local.h"
#include "dl_library.h"
#include "lfs.h"
#include "local_probe.h"
#include "local_subs.h"
#include "subtitles.h"
#include "mkv_ts.h"
#include "adec.h"
#include "video.h"
#include "surround.h"
#include "audio_bitstream.h"
#include "plog.h"

// A picture whose PTS is 0 reads as "no PTS", so a stream entered at a key frame starts its clock
// a little before it: mkv_ts does the same (MKV_TS_PTS_BASE_US).
#define LOCAL_PTS_LEAD_US  ((u64)MKV_TS_PTS_BASE_US)

// Probes read through their own descriptor, so the stream's position (and
// its buffer contents, which a probe borrows as scratch only while no
// stream is open) are never disturbed.
static int local_read_at(void *ctx, uint64_t offset, uint8_t *buf, int len) {
    const int h = *(const int *)ctx;
    const int got = lfs_read(h, offset, buf, (uint32_t)len);
    return got < 0 ? -1 : got;
}

static bool probe_open(const char *path, int *h) {
    *h = lfs_open(path);
    return *h >= 0;
}

// The audio track the session is on: an MKV track number or a TS PID.  0 for a download, which
// carries the one it was made with.
static int audio_id(const PlayerState *ps) {
    if (!ps->have_tracks || ps->cur_audio < 0 || ps->cur_audio >= ps->tracks.n_audio) return 0;
    return ps->tracks.audio[ps->cur_audio].index;
}

// The subtitle track the session is on, as the stream layer wants it: its track number (0 = off) and kind.
static bool sub_choice(const PlayerState *ps, int *track, LocalSubKind *kind) {
    *track = 0; *kind = LS_OTHER;
    if (!ps->have_tracks || ps->cur_sub < 0 || ps->cur_sub >= ps->tracks.n_subs) return false;
    *track = ps->tracks.subs[ps->cur_sub].index;
    *kind = ps->local->sub_kind[ps->cur_sub];
    return true;
}

static bool open_mkv(PlayerState *ps, u64 target_us) {
    const PlayerLocal *lp = ps->local;
    uint64_t actual_ns = 0;
    // The cues of the track in use start again from where the file is entered: each (re)open collects them afresh.
    int sub = 0;
    LocalSubKind sub_kind = LS_OTHER;
    if (sub_choice(ps, &sub, &sub_kind)) subs_local_begin(sub_kind == LS_PGS);
    else subs_clear();
    ps->sock = stream_open_mkv(lp->path, lp->video_id, audio_id(ps), sub, sub ? local_sub_sink : NULL,
                               (void *)(intptr_t)sub_kind, target_us * 1000ULL, &actual_ns);
    if (ps->sock < 0) return false;
    // mkv_ts starts the stream's clock at the key frame it entered on, with one audio track
    video_set_audio_pid(0);
    video_set_pts_origin_us(0);
    adec_set_pts_origin_us(0);
    const u64 at_us = actual_ns / 1000ULL;
    ps->play_base_us = at_us > LOCAL_PTS_LEAD_US ? at_us - LOCAL_PTS_LEAD_US : 0;
    char b[112];
    snprintf(b, sizeof(b), "local: mkv target=%llus entry=%llus audio=%d",
             (unsigned long long)(target_us / 1000000ULL), (unsigned long long)(at_us / 1000000ULL),
             audio_id(ps));
    plog(b);
    return true;
}

// A transport stream (a download, a .ts, a Blu-ray .m2ts): its entry point comes from the file.
static bool open_ts(PlayerState *ps, u64 target_us) {
    const PlayerLocal *lp = ps->local;
    const bool m2ts = lp->container == LM_M2TS;
    int fd;
    if (!probe_open(lp->path, &fd)) return false;
    LocalM2tsView view = { local_read_at, &fd };
    StreamReadAtFn rd = m2ts ? local_m2ts_read_at : local_read_at;
    void *ctx = m2ts ? (void *)&view : (void *)&fd;
    int cap = 0;
    u8 *scratch = stream_scratch(&cap);
    // Probe in 256 KB reads: small enough to be quick on the HDD, big
    // enough to hold a PAT/PMT/keyframe run in one go.
    uint64_t off = 0, at_us = 0;
    const bool ok = stream_local_seek(rd, ctx, &lp->idx, target_us, scratch, cap, &off, &at_us);
    lfs_close(fd);
    if (!ok) return false;
    ps->sock = m2ts ? stream_open_m2ts(lp->path, off) : stream_open_file(lp->path, off);
    if (ps->sock < 0) return false;

    // The stream's time starts at the entry, as a server stream's does (stream_local_clock).
    uint64_t origin = 0, base = 0;
    stream_local_clock(&lp->idx, at_us, LOCAL_PTS_LEAD_US, &origin, &base);
    video_set_audio_pid((u16)audio_id(ps));
    video_set_pts_origin_us(origin);
    adec_set_pts_origin_us(origin);
    ps->play_base_us = base;
    char b[128];
    snprintf(b, sizeof(b), "local: target=%llus entry=%llus offset=%llu audio=%d",
             (unsigned long long)(target_us / 1000000ULL),
             (unsigned long long)(at_us / 1000000ULL), (unsigned long long)off, audio_id(ps));
    plog(b);
    return true;
}

bool player_local_open(PlayerState *ps, u64 target_us) {
    if (ps->local->container == LM_MKV) return open_mkv(ps, target_us);
    return open_ts(ps, target_us);
}

bool show_player_offline(const char *item_id, u32 resume_secs) {
    // Verified at the moment of playing, not when some list was drawn: a
    // file removed or truncated since then must not reach the decoder.
    static DlLibraryEntry e;          // ~3 KB: off the stack
    if (!dl_library_get(item_id, &e)) {
        plog("offline: not playable (not completed, or media missing/changed)");
        return false;
    }
    static PlayerLocal local;
    memset(&local, 0, sizeof(local));
    snprintf(local.path, sizeof(local.path), "%s", e.media_path);
    snprintf(local.label, sizeof(local.label), "Offline");
    dl_library_plan(&e, &local.plan);

    // Measure the file itself: its first keyframe and its real duration.
    // A stale meta.txt cannot then mislead the HUD or the seek bar.
    int fd;
    if (!probe_open(local.path, &fd)) return false;
    int cap = 0;
    u8 *scratch = stream_scratch(&cap);
    const int64_t size = (int64_t)e.bytes;
    const bool indexed = stream_local_index(local_read_at, &fd, (uint64_t)size,
                                            scratch, cap, &local.idx);
    lfs_close(fd);
    if (!indexed) {
        plog("offline: media.ts has no video to enter");
        return false;
    }
    {
        char b[112];
        snprintf(b, sizeof(b), "offline: %.8s %ux%u dur=%us meta=%d light=%d",
                 item_id, local.plan.req_w, local.plan.req_h,
                 local.idx.duration_secs, (int)e.meta_ok, (int)local.plan.light);
        plog(b);
    }

    static JFItem item;
    memset(&item, 0, sizeof(item));
    snprintf(item.id,   sizeof(item.id),   "%s", e.meta.id);
    snprintf(item.name, sizeof(item.name), "%s", e.meta.title);
    snprintf(item.type, sizeof(item.type), "%s", e.meta.type);
    show_player_run(&item, resume_secs, NULL, &local);
    return true;
}

// ---- a file from a drive ------------------------------------------------------------------------

static void set_why(char *why, int cap, const char *text) {
    if (why && cap > 0) snprintf(why, (size_t)cap, "%s", text);
}

static const char *file_name_of(const char *path) {
    const char *n = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == ':') n = p + 1;
    return n;
}

bool show_player_file(const char *path, const char *title, u32 resume_secs, char *why, int why_cap) {
    static LocalInfo info;            // ~1.7 KB, ~3.5 KB: off the stack
    static PlayerLocal local;
    memset(&info, 0, sizeof(info));
    memset(&local, 0, sizeof(local));

    int fd = lfs_open(path);
    if (fd < 0) {
        set_why(why, why_cap, fd == LFS_E_REMOVED ? TR("The drive was removed.") : TR("The file could not be opened."));
        return false;
    }
    const uint64_t size = lfs_size(fd);
    char err[64] = "";
    const bool probed = local_probe(local_read_at, &fd, size, file_name_of(path), &info, err, sizeof err);
    lfs_close(fd);
    if (!probed) {
        set_why(why, why_cap, err[0] ? err : TR("This file could not be read."));
        return false;
    }

    // Refuse here, before anything is torn down for playback: HEVC and the like never reach the decoder.
    const LocalAudioPrefs prefs = { surround_enabled(), audio_passthrough_wanted() };
    if (!local_can_play(&info, &prefs, why, why_cap)) return false;
    const int pick = local_pick_audio(&info, &prefs);

    local.container = info.container;
    local.video_id  = info.video_id;
    snprintf(local.path, sizeof(local.path), "%s", path);
    snprintf(local.label, sizeof(local.label), "File");
    // The audio the player can decode, in the file's order; the HUD's AUDIO menu lists these.
    for (int i = 0; i < info.n_audio && local.tracks.n_audio < JF_MAX_STREAMS; i++) {
        if (!info.audio[i].decodable) continue;
        JFStream *s = &local.tracks.audio[local.tracks.n_audio];
        s->index = info.audio[i].id;
        snprintf(s->label, sizeof(s->label), "%.*s", local_clip_utf8(info.audio[i].label, (int)sizeof(s->label) - 1),
                 info.audio[i].label);
        if (i == pick) local.start_audio = local.tracks.n_audio;
        local.tracks.n_audio++;
    }
    local.tracks.default_audio = local.start_audio;
    local.have_tracks = local.tracks.n_audio > 0;
    // The subtitles of a Matroska file that the player can draw (text and PGS).  They are read as the file plays, so the
    // list is only what the probe found; a forced track in the audio's language starts on.
    local.start_sub = -1;
    if (info.container == LM_MKV) {
        const int want = local_pick_sub(&info, pick);
        for (int i = 0; i < info.n_subs && local.tracks.n_subs < JF_MAX_STREAMS; i++) {
            if (!info.subs[i].usable || !local_sub_kind_playable(info.subs[i].kind)) continue;
            JFStream *s = &local.tracks.subs[local.tracks.n_subs];
            s->index = info.subs[i].id;
            snprintf(s->label, sizeof(s->label), "%.*s", local_clip_utf8(info.subs[i].label, (int)sizeof(s->label) - 1),
                     info.subs[i].label);
            snprintf(s->codec, sizeof(s->codec), "%s", info.subs[i].kind == LS_PGS ? "pgssub" : info.subs[i].kind == LS_ASS ? "ass" : "subrip");
            local.sub_kind[local.tracks.n_subs] = info.subs[i].kind;
            if (i == want) local.start_sub = local.tracks.n_subs;
            local.tracks.n_subs++;
        }
    }

    // The frame ceiling the decoder and the jitter buffer are sized for: the picture itself.  A drive's
    // files are not "light" (the downloads wait for them).
    const bool dims_ok = info.width > 0 && info.height > 0 && info.width <= 1920 && info.height <= 1080;
    local.plan.req_w = dims_ok ? (u32)info.width  : 1920;
    local.plan.req_h = dims_ok ? (u32)info.height : 1080;
    local.plan.runtime_secs = info.duration_secs;
    local.plan.light = false;

    if (info.container == LM_MKV) {
        local.idx.duration_secs = info.duration_secs;
        local.idx.size = size;
    } else {
        // A transport stream is entered at a key frame found in the file itself.
        const bool m2ts = info.container == LM_M2TS;
        if (!probe_open(path, &fd)) {
            set_why(why, why_cap, TR("The file could not be opened."));
            return false;
        }
        LocalM2tsView view = { local_read_at, &fd };
        StreamReadAtFn rd = m2ts ? local_m2ts_read_at : local_read_at;
        void *ctx = m2ts ? (void *)&view : (void *)&fd;
        int cap = 0;
        u8 *scratch = stream_scratch(&cap);
        const bool indexed = stream_local_index(rd, ctx, m2ts ? local_m2ts_ts_size(size) : size,
                                                scratch, cap, &local.idx);
        lfs_close(fd);
        if (!indexed) {
            set_why(why, why_cap, TR("This file has no picture the player can start from."));
            return false;
        }
    }
    {
        char b[160];
        snprintf(b, sizeof(b), "file: %s %s %ux%u dur=%us audio=%d/%d start=%d", info.video_desc,
                 info.container == LM_MKV ? "mkv" : info.container == LM_M2TS ? "m2ts" : "ts",
                 local.plan.req_w, local.plan.req_h, local.idx.duration_secs ? local.idx.duration_secs : info.duration_secs,
                 local.tracks.n_audio, info.n_audio, local.start_audio);
        plog(b);
    }

    // No Jellyfin id: nothing in the player can then ask the server about it.
    static JFItem item;
    memset(&item, 0, sizeof(item));
    snprintf(item.name, sizeof(item.name), "%s", title && title[0] ? title : file_name_of(path));
    snprintf(item.type, sizeof(item.type), "Video");
    show_player_run(&item, resume_secs, NULL, &local);
    return true;
}
