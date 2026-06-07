#pragma once

#include <ppu-types.h>

// -------------------------------------------------------
// Thread context structs (player.cpp spawns, player_threads.cpp runs)
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

// -------------------------------------------------------
// GPU blit state (defined in player_gpu.cpp)
// -------------------------------------------------------

extern volatile u32 * s_vid_tex_buf[2];
extern volatile u32 * s_vid_tex_buf_b[2];
extern volatile int   s_vid_disp_idx;
extern volatile bool  s_vid_frame_ready;
extern volatile bool  s_vid_b_present;

void vid_gpu_init(u32 fw, u32 fh);
void vid_gpu_free(void);
void vid_gpu_draw(bool render_blend, float blend_factor, u32 fw, u32 fh);
