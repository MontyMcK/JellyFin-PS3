#pragma once

// Temporal crossfade between neighbouring film frames on a 59.94 Hz output.
// Off by default: a TV that detects 3:2 pulldown (film mode, motion
// smoothing) sees blended frames as a broken cadence, so whole frames with
// plain pulldown are what the player presents unless this is switched on.
// Persisted in jellyfin_blend.txt as "1" (on); missing or "0" is off.
bool vblend_enabled(void);
