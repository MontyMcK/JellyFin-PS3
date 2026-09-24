// AUDIO / CC track selection — opens the HUD popup menus and applies the
// chosen track.  Picking a different track reopens the stream at the current
// position with the new AudioStreamIndex/SubtitleStreamIndex.  The reopen IS
// the seek path with delta 0 — same stop-transcode + fresh-PlaySessionId
// dance, just different URL params.

#include <stdio.h>
#include <string.h>

#include "player_internal.h"
#include "plog.h"
#include "subtitles.h"
#include "slog.h"

// ---- Remembered track preference (see player_internal.h) ------------------
static char s_pref_audio_label[64] = "";
static char s_pref_sub_label[64]   = "";
static bool s_pref_sub_have        = false;   // user has chosen a sub state
static bool s_pref_sub_off         = false;   // and that choice was "Off"

void track_pref_note_audio(const JFStream *s) {
    if (!s) return;
    snprintf(s_pref_audio_label, sizeof(s_pref_audio_label), "%s", s->label);
}

void track_pref_note_sub(const JFStream *s) {
    s_pref_sub_have = true;
    s_pref_sub_off  = (s == NULL);
    if (s) snprintf(s_pref_sub_label, sizeof(s_pref_sub_label), "%s", s->label);
    else   s_pref_sub_label[0] = '\0';
}

int track_pref_find_audio(const JFTracks *tracks) {
    if (!s_pref_audio_label[0]) return -1;
    for (int i = 0; i < tracks->n_audio; i++)
        if (strcmp(tracks->audio[i].label, s_pref_audio_label) == 0)
            return i;
    return -1;
}

int track_pref_find_sub(const JFTracks *tracks) {
    if (!s_pref_sub_have || s_pref_sub_off || !s_pref_sub_label[0]) return -1;
    for (int i = 0; i < tracks->n_subs; i++)
        if (strcmp(tracks->subs[i].label, s_pref_sub_label) == 0 &&
            jf_sub_is_text(tracks->subs[i].codec))
            return i;
    return -1;
}

HudAction player_handle_menu_action(PlayerState *ps, HudAction act) {
    if (act == HUD_ACTION_AUDIO_TRACK) {
        act = HUD_ACTION_NONE;
        if (ps->have_tracks && ps->tracks.n_audio > 0) {
            const char *items[JF_MAX_STREAMS];
            for (int i = 0; i < ps->tracks.n_audio; i++)
                items[i] = ps->tracks.audio[i].label;
            ps->menu_kind = PLAYER_MENU_AUDIO;
            hud_open_menu("Audio", items, ps->tracks.n_audio, ps->cur_audio);
        } else {
            plog("hud: audio - no tracks");
        }
    } else if (act == HUD_ACTION_SUBTITLE) {
        act = HUD_ACTION_NONE;
        if (ps->have_tracks && ps->tracks.n_subs > 0) {
            const char *items[JF_MAX_STREAMS + 1];
            items[0] = "Off";
            for (int i = 0; i < ps->tracks.n_subs; i++)
                items[1 + i] = ps->tracks.subs[i].label;
            ps->menu_kind = PLAYER_MENU_SUBS;
            hud_open_menu("Subtitles", items, ps->tracks.n_subs + 1,
                          ps->cur_sub + 1);
        } else {
            plog("hud: subs - none available");
        }
    } else if (act == HUD_ACTION_MENU_SELECT) {
        act = HUD_ACTION_NONE;
        int sel = hud_menu_choice();
        if (ps->menu_kind == PLAYER_MENU_AUDIO &&
            sel >= 0 && sel < ps->tracks.n_audio && sel != ps->cur_audio) {
            ps->cur_audio = sel;
            track_pref_note_audio(&ps->tracks.audio[ps->cur_audio]);
            act = HUD_ACTION_SEEK;     // 0-delta reopen applies the track
            char buf[96];
            snprintf(buf, sizeof(buf), "hud: audio -> [%d] %s",
                     ps->tracks.audio[ps->cur_audio].index,
                     ps->tracks.audio[ps->cur_audio].label);
            plog(buf);
            slog_state("AUDIO_TRACK sel=%d idx=%d label=%.30s",
                       ps->cur_audio, ps->tracks.audio[ps->cur_audio].index,
                       ps->tracks.audio[ps->cur_audio].label);
        } else if (ps->menu_kind == PLAYER_MENU_SUBS &&
                   sel >= 0 && sel <= ps->tracks.n_subs &&
                   sel - 1 != ps->cur_sub) {
            ps->cur_sub = sel - 1;     // entry 0 = "Off" -> -1
            hud_set_cc_active(ps->cur_sub >= 0);

            // A TEXT or PGS track is fetched and drawn here, so it needs no
            // reopen at all -- the video stream is untouched and carries on.
            // Only a track this app cannot decode itself (VOBSUB, or a
            // fetch failure) changes the URL to ask for burn-in, and only
            // that case is worth interrupting playback for.
            ps->sub_is_text = false;
            ps->sub_is_pgs  = false;
            if (ps->cur_sub >= 0) {
                const JFStream *st = &ps->tracks.subs[ps->cur_sub];
                ps->sub_is_text = jf_sub_is_text(st->codec);
                if (ps->sub_is_text) {
                    const char *msid = ps->source.id;
                    if (subs_load(ps->item->id, msid, st->index) > 0) {
                        ps->menu_kind = PLAYER_MENU_NONE;
                        track_pref_note_sub(st);
                        char b[112];
                        snprintf(b, sizeof(b),
                                 "subs: drawing [%d] %.40s on-device, no reopen",
                                 st->index, st->label);
                        plog(b);
                        return act;            // nothing to re-negotiate
                    }
                    // Could not fetch it: fall through to the burn-in path
                    // rather than silently showing nothing.
                    plog("subs: on-device load failed, asking the server to burn in");
                    ps->sub_is_text = false;
                } else if (jf_sub_is_pgs(st->codec)) {
                    const char *msid = ps->source.id;
                    if (subs_load_pgs(ps->item->id, msid, st->index) > 0) {
                        ps->sub_is_pgs = true;
                        ps->menu_kind  = PLAYER_MENU_NONE;
                        // Not remembered via track_pref_note_sub(): that
                        // preference match requires jf_sub_is_text() (see
                        // player_internal.h), so a PGS pick is deliberately
                        // never auto-applied to the next title -- it would
                        // have to fetch a whole .sup speculatively to find
                        // out whether the match was even right.
                        char b[112];
                        snprintf(b, sizeof(b),
                                 "subs: drawing pgs [%d] %.40s on-device, no reopen",
                                 st->index, st->label);
                        plog(b);
                        return act;
                    }
                    plog("subs: pgs load failed, asking the server to burn in");
                }
            } else {
                subs_clear();
                track_pref_note_sub(NULL);
            }
            act = HUD_ACTION_SEEK;     // 0-delta reopen applies the sub
            char buf[96];
            if (ps->cur_sub >= 0)
                snprintf(buf, sizeof(buf), "hud: subs -> [%d] %s",
                         ps->tracks.subs[ps->cur_sub].index,
                         ps->tracks.subs[ps->cur_sub].label);
            else
                snprintf(buf, sizeof(buf), "hud: subs -> off");
            plog(buf);
            if (ps->cur_sub >= 0)
                slog_state("SUB_TRACK sel=%d idx=%d label=%.30s", ps->cur_sub,
                           ps->tracks.subs[ps->cur_sub].index,
                           ps->tracks.subs[ps->cur_sub].label);
            else
                slog_state("SUB_TRACK sel=-1 off=1");
        }
        ps->menu_kind = PLAYER_MENU_NONE;
    }
    return act;
}
