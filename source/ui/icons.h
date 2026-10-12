#pragma once

// Icon codepoints for the UI icon font — Tabler Icons v3.44 (MIT).
// The embedded font (source/ui/fonts/tabler_icons.h) is a subset holding
// exactly these glyphs; adding an icon here means re-subsetting the font
// (see the header's regeneration note).
//
// Names match tabler.io/icons so new glyphs are easy to look up.

#define ICON_SEARCH        0xEB1C   // search
#define ICON_HOME          0xEAC1   // home
#define ICON_MOVIE         0xEAFA   // movie
#define ICON_TV            0xEA8D   // device-tv
#define ICON_ANTENNA       0xF094   // antenna: Live TV (the tab for TV shows keeps ICON_TV)
#define ICON_MUSIC         0xEAFC   // music
#define ICON_COLLECTIONS   0xEEF7   // stack-2
#define ICON_SETTINGS      0xEB20   // settings
#define ICON_BACKSPACE     0xEA2D   // backspace
#define ICON_LOGOUT        0xEBA8   // logout
#define ICON_BUG           0xEA48   // bug
#define ICON_STAR          0xEB2E   // star
#define ICON_PHOTO         0xEB0A   // photo
#define ICON_CHEVRON_RIGHT 0xEA61   // chevron-right
#define ICON_SHUFFLE       0xF000   // arrows-shuffle
#define ICON_SKIP_BACK     0xED48   // player-skip-back
#define ICON_SKIP_FWD      0xED49   // player-skip-forward
#define ICON_PLAY          0xED46   // player-play
#define ICON_PAUSE         0xED45   // player-pause
#define ICON_SEEK_BACK     0xEA64   // chevrons-left
#define ICON_SEEK_FWD      0xEA65   // chevrons-right

// The Settings categories (ui/render/ui_settings.cpp): one glyph each, so no two
// categories share an icon.  Playback uses ICON_PLAY, System ICON_SETTINGS.
#define ICON_VOLUME        0xEB4F   // volume-2: Audio
#define ICON_CAPTIONS      0xF55A   // badge-cc: Subtitles
#define ICON_DOWNLOAD      0xEA96   // download: Downloads
#define ICON_DESKTOP       0xEA89   // device-desktop: Display
