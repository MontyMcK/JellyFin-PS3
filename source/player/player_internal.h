#pragma once

#include <ppu-types.h>
#include <sys/thread.h>

#include "jellyfin_api.h"
#include "player_hud.h"

// -------------------------------------------------------
// Thread context structs (core/player.cpp spawns, threads/player_threads.cpp runs)
// -------------------------------------------------------

struct DecodeCtx {
    volatile bool *playing;
    int           *frame_count;  // read-only for heartbeat (benign race)
    int            sock;
    volatile bool *dec_run;      // seek-only stop flag; cleared to pause just this thread
};

struct AudioCtx {
    volatile bool *playing;
    volatile bool *paused;
};

struct UploadCtx {
    volatile bool *playing;
    u32 fw, fh;
};

void decode_thread_fn(void *arg);
void audio_thread_fn(void *arg);
void upload_thread_fn(void *arg);

// Playback-state reporter — posts position to Jellyfin every ~10 s so the
// server's Continue Watching list tracks PS3 playback.  arg = PlayerState*.
void progress_thread_fn(void *arg);

// -------------------------------------------------------
// Per-session player state, shared between the core/ files.
// One instance lives in show_player() for the playback session.
// -------------------------------------------------------

// Which popup menu the HUD is showing — decides what a
// HUD_ACTION_MENU_SELECT applies to.
enum { PLAYER_MENU_NONE, PLAYER_MENU_AUDIO, PLAYER_MENU_SUBS };

// R2/L2 tap/hold seek input state (core/player_seek.cpp).
enum { SEEK_IDLE, SEEK_PRESS, SEEK_SCRUB };
struct PlayerSeekInput {
    int  state;             // SEEK_IDLE / SEEK_PRESS / SEEK_SCRUB
    int  dir;               // +1 = fwd, -1 = back
    u64  press_us;          // when the current press began
    u64  active_us;         // last frame the trigger read as down
    u64  scrub_step_us;     // last scrub accumulation time
    s32  pending_secs;      // queued offset, seconds
    u64  tap_gate_us;       // tap batch deadline (0 => none)
    bool commit_now;        // release -> commit the reopen now
    bool scrub_resume;      // was playing when this scrub began
    bool resume_after_seek; // unpause once the seek lands
};

struct PlayerState {
    const JFItem *item;
    u32      req_w, req_h;       // transcode size (H.264 level 3.1 cap)
    char     session_id[64];     // Jellyfin PlaySessionId (re-minted per seek)
    unsigned total_secs;         // item runtime (0 = unknown)

    // Selectable audio/subtitle tracks (HUD AUDIO + CC popup menus).
    // cur_* are positions in the tracks arrays; -1 = server default / off.
    JFTracks tracks;
    bool     have_tracks;
    int      cur_audio;
    int      cur_sub;
    // True when cur_sub names a TEXT track this app draws itself rather than
    // one the server has to burn in.  Decided from the stream's Codec at
    // selection time; see build_stream_url() for why it changes the URL.
    bool     sub_is_text;
    // Same idea for a PGS bitmap track drawn on-device (subtitles_pgs.h) --
    // a separate flag rather than folding into sub_is_text because the
    // fetch/decode/draw paths are entirely different, but build_stream_url()
    // treats both identically: either one means "do not ask for burn-in".
    bool     sub_is_pgs;
    int      menu_kind;          // PLAYER_MENU_*

    // The version chosen before playback.  Only the active source is retained;
    // the full list belongs to the info screen and never enters the player.
    JFMediaSource source;

    int      sock;
    volatile bool playing;
    volatile bool paused;
    volatile bool dec_run;       // decode-thread stop flag (toggled on seek)
    int      frame_count;

    // Absolute media time (us) corresponding to the current stream's PTS 0.
    // Jellyfin restarts the transcode PTS at 0 on the initial open and on every
    // seek, so the audio clock alone is stream-relative. Absolute position is
    // play_base_us + audio_get_clock_us(); the base advances on each seek.
    u64      play_base_us;
    int      seek_dbg_frames;    // >0: log HUD elapsed for this many frames
    // When a seek lands while the video is PAUSED, the normal display gate
    // (which requires !paused) won't swap in the new frame, so the screen would
    // stay frozen on the pre-seek image.  This one-shot forces a single flip
    // after a paused seek so the user sees the target position.
    bool     show_seek_frame;
    u64      det_timeout_start;  // fps-detection timeout for Bresenham fallback

    DecodeCtx        dec_ctx;
    sys_ppu_thread_t dec_tid;

    PlayerSeekInput seek;
};

// -------------------------------------------------------
// core/player_session.cpp — session helpers
// -------------------------------------------------------

// Wait-for-O error screen.
void show_error(const char *line1, const char *line2);

// plog() truncates every line at 127 chars; log long URLs in ~100-char chunks.
void plog_url(const char *tag, const char *url);

// Jellyfin MediaStream index of the current audio/subtitle selection (-1 = none).
int  player_audio_stream_idx(const PlayerState *ps);
int  player_sub_stream_idx(const PlayerState *ps);
const JFMediaSource *player_current_source(const PlayerState *ps);

// Build the transcode stream URL — used for the initial open and every seek.
// start_ticks is in Jellyfin's 100-ns units (seconds * 10,000,000).
void build_stream_url(char *url, int url_sz, const PlayerState *ps,
                      u64 start_ticks);

// Feed TS packets until the jitter buffer holds JBUF_PREFILL frames.
// fatal_on_eof: a network error stops playback (initial open) instead of
// just ending the prefill (seek re-prime).  guard_max bounds the loop.
void player_prefill(PlayerState *ps, bool fatal_on_eof, int guard_max);

// -------------------------------------------------------
// core/player_menu.cpp — AUDIO / CC track popup handling
// -------------------------------------------------------

// Handle HUD_ACTION_AUDIO_TRACK / SUBTITLE / MENU_SELECT.  Returns the action
// still to perform: HUD_ACTION_SEEK when a track change needs a 0-delta
// reopen, otherwise HUD_ACTION_NONE (or act unchanged if not menu-related).
HudAction player_handle_menu_action(PlayerState *ps, HudAction act);

// Remembered audio/subtitle track choice, for the rest of this app run (not
// persisted to disk -- a binge session is the whole point; surviving a
// relaunch is not). Matched by exact JFTracks label rather than index or
// language, because Jellyfin's DisplayTitle is stable across episodes of the
// same show while stream indices are not, and a fuzzy match risks silently
// landing on the wrong track. Only ever set from a track the user actually
// picked (see player_menu.cpp), so applying it can never surprise the user
// with a track they did not choose themselves at some point this session.
void track_pref_note_audio(const JFStream *s);
void track_pref_note_sub(const JFStream *s);   // NULL = "subtitles off"

// Index into tracks->audio[]/subs[] matching the remembered preference, or
// -1 if there is none yet or nothing in this title matches. The subtitle
// match additionally requires a TEXT codec (jf_sub_is_text) -- a remembered
// preference must never silently trigger the burn-in transcode path.
int  track_pref_find_audio(const JFTracks *tracks);
int  track_pref_find_sub(const JFTracks *tracks);

// -------------------------------------------------------
// core/player_seek.cpp — seek input machine + seek execution
// -------------------------------------------------------

// Queue one HUD-originated tap (X on Rew/FF) into the pending-seek batch.
void player_seek_queue_tap(PlayerState *ps, int delta_secs);

// Run the R2/L2 tap/hold state machine and the commit gate for one frame.
// Returns HUD_ACTION_SEEK when the accumulated seek should fire now.
HudAction player_seek_input_update(PlayerState *ps, HudAction act);

// Execute the pending seek: stop decode thread, flush, stop the server
// transcode, mint a fresh PlaySessionId, reopen at the target, re-prime,
// respawn decode.  Returns false when playback must abort.
bool player_execute_seek(PlayerState *ps);

// -------------------------------------------------------
// core/player_display.cpp — per-vblank display step
// -------------------------------------------------------

// Movian-style duration-consumption gate, frame swap, GPU draw, HUD overlay,
// and the playback diagnostics.  Call once per display-loop iteration.
void player_display_frame(PlayerState *ps);

// -------------------------------------------------------
// threads/player_threads.cpp — decode-thread spawn (initial + post-seek)
// -------------------------------------------------------

bool player_spawn_decode(PlayerState *ps);

// Compressed read-ahead ring (player_threads.cpp).  Allocated per playback
// from whatever memory is free and released on teardown; this is the buffer
// that rides out a server delivering its transcode below real time, so a
// bigger one is strictly better until it starves the rest of the app.
// decode_ring_fill()/cap() are in PACKETS and drive the pre-roll wait.
bool decode_ring_alloc(u32 want_bytes);
void decode_ring_free(void);
int  decode_ring_fill(void);
int  decode_ring_cap(void);

// -------------------------------------------------------
// GPU blit state (defined in gpu/player_gpu.cpp)
// -------------------------------------------------------

// Planar YUV plane textures: [dbuf][plane 0=Y 1=Cb 2=Cr].
extern volatile u8  * s_yuvA_buf[2][3];  extern u32 s_yuvA_off[2][3];
extern volatile u8  * s_yuvB_buf[2][3];  extern u32 s_yuvB_off[2][3];
extern volatile int   s_vid_disp_idx;
extern volatile bool  s_vid_frame_ready;
extern volatile u32   s_vid_uploaded_seq;
extern volatile bool  s_vid_b_present;

void vid_gpu_init(u32 fw, u32 fh);
void vid_gpu_free(void);
void vid_gpu_draw(bool render_blend, float blend_factor, u32 fw, u32 fh);
