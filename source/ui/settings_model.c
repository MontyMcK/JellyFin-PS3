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

int settings_category_count(void) {
    int n = 0;
    for (int s = 0; s < SEC__COUNT; s++)
        if (settings_section_first((setting_section)s) >= 0) n++;
    return n;
}

setting_section settings_category_at(int i) {
    int n = 0;
    for (int s = 0; s < SEC__COUNT; s++)
        if (settings_section_first((setting_section)s) >= 0) {
            if (n == i) return (setting_section)s;
            n++;
        }
    return SEC__COUNT;
}

int settings_category_index(setting_section s) {
    if (s < 0 || s >= SEC__COUNT || settings_section_first(s) < 0) return -1;
    int n = 0;
    for (int t = 0; t < (int)s; t++)
        if (settings_section_first((setting_section)t) >= 0) n++;
    return n;
}

int settings_category_row_count(setting_section s) {
    int n = 0;
    for (int i = 0; i < N_ROWS; i++) if (k_rows[i].section == s) n++;
    return n;
}

int settings_category_row(setting_section s, int k) {
    if (k < 0) return -1;
    int n = 0;
    for (int i = 0; i < N_ROWS; i++)
        if (k_rows[i].section == s) {
            if (n == k) return i;
            n++;
        }
    return -1;
}

int settings_row_in_category(int row) {
    if (row < 0 || row >= N_ROWS) return -1;
    int k = 0;
    for (int i = 0; i < row; i++) if (k_rows[i].section == k_rows[row].section) k++;
    return k;
}

int settings_scroll_to(int n, int sel, int item_h, int gap, int band_h, int cur) {
    if (n <= 0 || sel < 0 || sel >= n) return cur < 0 ? 0 : cur;
    const int total = n * item_h + (n - 1) * gap;
    if (total <= band_h) return 0;
    const int top = sel * (item_h + gap), bot = top + item_h;
    int s = cur;
    if (top < s)          s = top;
    if (bot - band_h > s) s = bot - band_h;
    const int max_s = total - band_h;
    if (s > max_s) s = max_s;
    if (s < 0) s = 0;
    return s;
}
