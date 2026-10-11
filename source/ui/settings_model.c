// Settings screen model -- see settings_model.h.

#ifndef ENABLE_PLAYER_STATS
#include "../build_config.h"
#endif
#include "settings_model.h"
#include "update_page.h"
#include "i18n.h"

#include <stddef.h>

// The help text is exactly two lines: the first says what the setting does,
// the second what it means for the listener.
static const setting_row k_rows[] = {
    // ---- Playback
    { SET_HD1080,    SEC_PLAYBACK, TRN("1080p Playback (Alpha)"),
      TRN("Ask the server for full 1920x1080 video instead of 720p.\n"
      "Sharper, but needs more bandwidth. Still experimental.")},
    { SET_24HZ,      SEC_PLAYBACK, TRN("24Hz Output"),
      TRN("Auto: 24fps films switch the TV to 24Hz for smooth motion.\n"
      "Each TV is checked once first. Off keeps everything at 60Hz.")},
    { SET_DIRECT_STREAM, SEC_PLAYBACK, TRN("Direct Stream"),
      TRN("Auto: the server may pass a compatible video through untouched.\n"
      "Off: always re-encode. Try Off if playback stutters at every quality.")},
    { SET_AUTOSKIP,  SEC_PLAYBACK, TRN("Auto Skip"),
      TRN("Skip intros and recaps automatically, without pressing X.\n"
      "Credits start the next episode's 25 second countdown instead.")},
    // ---- Audio
    { SET_AUDIO_OUT, SEC_AUDIO, TRN("Audio Output"),
      TRN("Stereo, 5.1 or 7.1 over HDMI. Dolby Digital sends the film's Dolby\n"
      "track untouched: use it if your soundbar drops the centre channel.")},
    { SET_DIALOGUE,  SEC_AUDIO, TRN("Dialogue Boost"),
      TRN("Raise the centre channel, where the dialogue is. Not available\n"
      "with Dolby Digital passthrough. Helps when voices are quiet.")},
    // ---- Subtitles
    { SET_SUB_FONT,  SEC_SUBTITLES, TRN("Subtitle Font"),
      TRN("The typeface subtitles are drawn in.\n"
      "Left and Right step through the choices.")},
    { SET_SUB_COLOUR, SEC_SUBTITLES, TRN("Subtitle Colour"),
      TRN("The colour subtitles are drawn in.\n"
      "White is the broadcast default; the others are softer.")},
    // ---- Downloads
    { SET_DL_LOCATION, SEC_DOWNLOADS, TRN("Download Location"),
      TRN("Where new downloads are saved: this PS3's HDD, or an NTFS USB drive.\n"
      "The lists show the downloads on the drive while it is plugged in.")},
    { SET_DOWNLOADS, SEC_DOWNLOADS, TRN("Downloads"),
      TRN("Downloads in progress, with pause and cancel.\n"
      "They wait while you stream and carry on by themselves.")},
    { SET_OFFLINE_LIB, SEC_DOWNLOADS, TRN("Offline Library"),
      TRN("Films and episodes saved on this PS3, ready to play.\n"
      "They play without a connection to your server.")},
    // ---- Display
    { SET_SCREEN_SIZE, SEC_DISPLAY, TRN("Screen Size"),
      TRN("Shrink the picture to fit TVs that crop the edges (overscan).\n"
      "Line the corners up with your screen's edges.")},
    { SET_LANGUAGE,  SEC_DISPLAY, TRN("Language"),
      TRN("The language the menus are shown in.\n"
      "Auto follows the PS3's own language setting.") },
    { SET_THEME,     SEC_DISPLAY, TRN("Theme"),
      TRN("The interface colour theme.\n"
      "Jellywave is the default; extra .ini themes appear here too.")},
    { SET_DAYNIGHT,  SEC_DISPLAY, TRN("Day / Night Palette"),
      TRN("Brighten the background by day, back to the night look after\n"
      "dark, with a glow at dawn and dusk. Follows the console clock.")},
    { SET_WAVE_INT,  SEC_DISPLAY, TRN("Wave Intensity"),
      TRN("How strongly the wave reacts to music.\n"
      "Off keeps it calm; Max makes every beat hit hard.")},
    { SET_PARTICLES, SEC_DISPLAY, TRN("Menu Particles"),
      TRN("Let the floating particles drift behind the menus too,\n"
      "not only while music is playing.")},
    // ---- System
    { SET_UPDATE,    SEC_SYSTEM, TRN("Software Update"),
      TRN("Checked at every launch. X checks again. New versions:\n"
      UPDATE_PAGE_TEXT) },
    { SET_SEND_LOG,  SEC_SYSTEM, TRN("Send Log to Server"),
      TRN("Send the diagnostic log to your Jellyfin server (X). It is saved\n"
      "under Dashboard > Logs as upload_PS3_...log: share that file.")},
    { SET_WOL_MAC,   SEC_SYSTEM, TRN("Server MAC Address"),
      TRN("The server PC's MAC address, so this PS3 can wake it (X to enter).\n"
      "On the PC: ipconfig /all, then Physical Address. Jellyfin can't say.")},
    { SET_WAKE,      SEC_SYSTEM, TRN("Wake Server"),
      TRN("Send a Wake-on-LAN signal to the server PC (X). It is also sent when\n"
      "the server can't be reached. The PC must allow wake-up in its BIOS.")},
    { SET_DEBUG_LOG, SEC_SYSTEM, TRN("Debug Logging"),
      TRN("Write a diagnostic log to /dev_hdd0/tmp/player_log.txt (on by default).\n"
      "Use Send Log to Server when reporting a problem.")},
#if ENABLE_PLAYER_STATS
    { SET_STATS,     SEC_SYSTEM, TRN("Player Stats Overlay"),
      TRN("Show playback statistics over the video:\n"
      "frame rate, bitrate, buffer and decoder figures.")},
#endif
    { SET_LOGOUT,    SEC_SYSTEM, TRN("Log Out"),
      TRN("Sign out of this Jellyfin account.\n"
      "You will need to log in again to browse your library.")},
};

#define N_ROWS ((int)(sizeof(k_rows) / sizeof(k_rows[0])))

static const char *const k_section_labels[SEC__COUNT] = {
    TRN("Playback"), TRN("Audio"), TRN("Subtitles"), TRN("Downloads"), TRN("Display"), TRN("System"),
};

int settings_count(void) { return N_ROWS; }

const setting_row *settings_row(int i) {
    return (i >= 0 && i < N_ROWS) ? &k_rows[i] : NULL;
}

int settings_index_of(setting_id id) {
    for (int i = 0; i < N_ROWS; i++)
        if (k_rows[i].id == id) return i;
    return -1;
}

const char *settings_section_label(setting_section s) {
    return (s >= 0 && s < SEC__COUNT) ? k_section_labels[s] : "";
}

int settings_section_first(setting_section s) {
    for (int i = 0; i < N_ROWS; i++)
        if (k_rows[i].section == s) return i;
    return -1;
}

int settings_section_jump(int i, int dir) {
    if (i < 0 || i >= N_ROWS || dir == 0) return -1;
    int s = (int)k_rows[i].section;
    for (s += dir > 0 ? 1 : -1; s >= 0 && s < SEC__COUNT; s += dir > 0 ? 1 : -1) {
        const int first = settings_section_first((setting_section)s);
        if (first >= 0) return first;
    }
    return -1;
}

int settings_layout(settings_item *out, int max, int header_h, int row_h,
                    int row_pitch, int *total_h) {
    int n = 0, y = 0;
    int prev = -1;
    for (int i = 0; i < N_ROWS; i++) {
        if ((int)k_rows[i].section != prev) {
            prev = (int)k_rows[i].section;
            if (n < max) {
                out[n].is_header = 1; out[n].index = prev;
                out[n].y = y; out[n].h = header_h;
                n++;
            }
            y += header_h;
        }
        if (n < max) {
            out[n].is_header = 0; out[n].index = i;
            out[n].y = y; out[n].h = row_h;
            n++;
        }
        y += row_pitch;
    }
    // The last row has no gap below it.
    if (N_ROWS > 0) y -= (row_pitch - row_h);
    if (total_h) *total_h = y;
    return n;
}

int settings_item_of_row(const settings_item *items, int n, int row) {
    for (int k = 0; k < n; k++)
        if (!items[k].is_header && items[k].index == row) return k;
    return -1;
}

int settings_scroll_for(const settings_item *items, int n, int sel,
                        int band_h, int cur, int total_h) {
    const int k = settings_item_of_row(items, n, sel);
    if (k < 0) return cur < 0 ? 0 : cur;
    int top = items[k].y;
    const int bot = items[k].y + items[k].h;
    // The first row of a section brings its header with it.
    if (k > 0 && items[k - 1].is_header) top = items[k - 1].y;

    int s = cur;
    if (top < s)            s = top;
    if (bot - band_h > s)   s = bot - band_h;
    const int max_s = total_h > band_h ? total_h - band_h : 0;
    if (s > max_s) s = max_s;
    if (s < 0) s = 0;
    return s;
}
