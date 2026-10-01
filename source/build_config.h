#pragma once

// =========================================================================
//  BUILD TARGET SWITCH
// =========================================================================
//  1 = RPCS3 emulator test build
//  0 = real PS3 hardware build  <-- ALWAYS 0 FOR RELEASES
//
//  This is the ONE flag to flip when moving between the emulator and the
//  console.  It is a COMPILE-TIME switch on purpose: the runtime probe that
//  tried to detect the emulator (timing a full-screen CPU write) misread
//  retail hardware and put a real PS3 on the slow path — do not bring that
//  back (see ui_wave.cpp for the full story).
//
//  What it currently switches (all behind ui_cpu_bg() in ui_wave.cpp):
//   - XMB background compositing: RPCS3 presents its cached GPU surface on
//     flip and silently drops CPU framebuffer writes, so emulator builds
//     must composite the ENTIRE frame (background included) on the CPU.
//     Hardware builds keep the GPU wave — CPU-filling uncached VRAM on a
//     real PS3 costs ~150-180ms/frame and the UI crawls.
//
//  NOT code: the other emulator differences live outside the app —
//  RPCS3's global config.yml needs `Internet enabled: Connected` +
//  `Bind address: <host LAN IP>`, and RPCS3 has its own separate
//  dev_hdd0/tmp/jellyfin_config.txt (keep the server URL on the LAN).
// =========================================================================

#define BUILD_FOR_RPCS3 0

// =========================================================================
//  PLAYER STATS INSTRUMENTATION
// =========================================================================
//  1 = compile the playback diagnostics module (player_stats.cpp) and the
//      Settings row that toggles its overlay
//  0 = strip it entirely — the metric hooks become inline no-ops, the ring
//      buffer / histogram storage vanishes, and the Settings row disappears
//
//  This is SEPARATE from the runtime switch (Settings > Player Stats
//  Overlay, stored by statsovl.cpp).  The runtime switch controls DISPLAY
//  only: with it off the counters still update, so flipping it on mid-movie
//  shows warm numbers immediately.  This flag controls whether the
//  MEASUREMENT exists at all, so a release build can drop the code and its
//  ~33 KB of sample storage without touching any call site.
//
//  Cost when 1 but the overlay is off: a handful of integer ops in the
//  vblank handler and one per presented frame.  Nothing is drawn and no
//  text layout runs — see player_stats_render_overlay().
// =========================================================================

#define ENABLE_PLAYER_STATS 1

// =========================================================================
//  STANDALONE PLAYLISTS TAB
// =========================================================================
//  0 = don't give Jellyfin's "playlists" library a tab of its own (default)
//  1 = show it alongside the other libraries
//
//  Jellyfin auto-creates a "Playlists" view for any user who has playlists,
//  so once every library got its own tab it started appearing in the bar —
//  duplicating content the Music tab already reaches through its
//  Albums/Artists/Playlists/Genres/Songs sub-tabs.
//
//  This suppresses the TAB only.  Detection still classifies the library as
//  TABKIND_PLAYLISTS rather than letting it fall through to a custom folder,
//  so if it is switched back on it browses correctly (drills into a
//  playlist's tracks, square cards) instead of being mistaken for a generic
//  library and handed to the video player.
// =========================================================================

#define SHOW_PLAYLISTS_TAB 0

// =========================================================================
//  CRT DISPLAY MODE
// =========================================================================
//  0 = tuned for flat-panel / LCD (gamma-2.0 AA, Open Sans Regular for body)
//  1 = tuned for CRT / interlaced displays (gamma-2.2 AA, bold weight for
//      text at or below the minimum size).  Thicker strokes cut interlace
//      flicker; the slightly higher gamma matches CRT phosphor response.
//
//  This is a COMPILE-TIME switch.  If someone ships both builds they should
//  version the PKG filename so users know which to grab.
// =========================================================================

#define CRT_DISPLAY 1
