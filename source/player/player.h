#pragma once
#include <ppu-types.h>
#include "jellyfin_api.h"

// Show the "now playing" screen for the given item.
// resume_secs > 0 starts playback at that position (Continue Watching).
// Blocks until the user presses START to go back.
// media_source_id selects a version before the stream opens; NULL/empty uses
// Jellyfin's default.  Sources are intentionally not switchable mid-playback.
void show_player(const JFItem *item, u32 resume_secs = 0,
                 const char *media_source_id = NULL);

// Offline playback (Stage 4): play a completed download from the HDD
// through this same player, with no server call of any kind.  The item is
// re-verified here (COMPLETED, media.ts present at its verified size, whole
// TS packets); anything else returns false without touching the player.
// resume_secs starts part way (the file is entered at the nearest keyframe).
// Blocks like show_player().  Nothing calls this yet: the Offline section
// that launches it is Stage 5.
bool show_player_offline(const char *item_id, u32 resume_secs = 0);

// A video file from the internal disk or a USB drive (an lfs path: "usb0:/Films/x.mkv"):
// Matroska, MPEG-TS or Blu-ray .m2ts, H.264 with AC-3, DTS, TrueHD or MPEG audio.  Plays
// through the same player with no server call of any kind; `title` is what the HUD shows.
// Returns false without having started anything when the file cannot be played, with the
// reason in `why` (may be NULL) in words fit for the screen.  Problems after playback has
// begun show their own error screen and still return true.  Blocks like show_player().
bool show_player_file(const char *path, const char *title, u32 resume_secs,
                      char *why, int why_cap);

// Where the last playback ended, in whole seconds (0 for none).  The Media browser stores it
// as the resume point.
u32 player_last_position_secs(void);
// The runtime of that playback in seconds (0 = unknown).
u32 player_last_total_secs(void);

// End-of-item auto-advance.  Arm before show_player() when the item has a
// follower: during the last 90 s of playback the player shows a popup badge
// reading `label` (with the instruction line `hint` drawn separately below
// it), and SELECT ends playback with the next-request flag set.  Episodes
// additionally auto-advance — the last 30 s show a countdown that sets the
// flag when it reaches zero (or when the stream EOFs inside that window).
// The armed state is consumed by the next show_player() call;
// player_take_next_request() returns and clears the flag.
void player_arm_next(const char *label, const char *hint);

// Episode chains (2026-09-26): what the last playback used, so the next
// episode keeps the same audio LANGUAGE and a matching version.
// player_chain_begin() clears it; each show_player() records into it and, if
// a chain is running, prefers the recorded language over the server default.
void        player_chain_begin(void);
void        player_chain_end(void);
const char *player_chain_source_label(void);   // "" when none
bool player_take_next_request(void);

// Live TV: the channel playing when the last live playback ended (channel up
// and down move it), "" before the first.  The Live TV list selects it on return.
const char *player_live_last_channel(void);
