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

#define BUILD_FOR_RPCS3 1
