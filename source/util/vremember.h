#pragma once
// -------------------------------------------------------------------------
//  Warm the server (api/api_warm.cpp)
// -------------------------------------------------------------------------
//  Ask for PlaybackInfo in the background the moment a details page opens,
//  so the server's first-time stream lookup (Gelato: 6-8 s) is done by the
//  time Play is pressed.  Fire-and-forget, one at a time, own buffer.
//
//  (The per-title remembered version that lived here was removed 2026-09-27
//  at the user's request, with the PlaybackInfo version fallback.)
void jellyfin_warm_playback(const char *item_id);
