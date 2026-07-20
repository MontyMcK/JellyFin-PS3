// AUDIO / CC track selection — opens the HUD popup menus and applies the
// chosen track.  Picking a different track reopens the stream at the current
// position with the new AudioStreamIndex/SubtitleStreamIndex.  The reopen IS
// the seek path with delta 0 — same stop-transcode + fresh-PlaySessionId
// dance, just different URL params.

#include <stdio.h>

#include "player_internal.h"
#include "plog.h"
#include "slog.h"

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
    }
    return act;
}
