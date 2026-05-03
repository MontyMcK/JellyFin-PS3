#include "audio.h"
#include "plog.h"

#include <stdio.h>
#include <string.h>
#include <ppu-types.h>
#include <audio/audio.h>
#include <sys/event_queue.h>

static u32               s_audio_port      = 0;
static sys_event_queue_t s_audio_eq        = {0};
static sys_ipc_key_t     s_audio_key       = 0;
bool                     s_audio_ok        = false;
static u32               s_read_index_addr = 0;
static u32               s_data_start      = 0;
static u32               s_num_blocks      = 0;

// Total audio blocks consumed since port start.  Incremented once per
// sysEventQueueReceive success in audio_silence().  Each block = 256 samples
// at 48 kHz = 5.333 ms.  Useful for A/V sync diagnostics.
static volatile u64 s_audio_blocks = 0;

u64 audio_block_count(void) { return s_audio_blocks; }

void audio_open(void) {
    if (audioInit() != 0) return;

    audioPortParam p;
    p.numChannels = AUDIO_PORT_2CH;
    p.numBlocks   = AUDIO_BLOCK_8;
    p.attrib      = 0;
    p.level       = 1.0f;
    if (audioPortOpen(&p, &s_audio_port) != 0) { audioQuit(); return; }

    // Per PSL1GHT docs the correct sequence is:
    //   Open → GetPortConfig → CreateEventQueue → SetEventQueue → Start
    // GetPortConfig must be called before Start; audioDataStart is valid
    // as soon as the port is opened.
    audioPortConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (audioGetPortConfig(s_audio_port, &cfg) == 0 && cfg.audioDataStart) {
        u32 nb = (u32)p.numBlocks;  // known value — don't trust cfg.numBlocks yet
        s_read_index_addr = cfg.readIndex;   // address of live SPU read-position u32
        s_data_start      = cfg.audioDataStart;
        s_num_blocks      = nb;
        // Zero the entire DMA ring.  Hardware reads zeros → digital silence.
        memset((void*)(uintptr_t)cfg.audioDataStart, 0,
               nb * 2 * AUDIO_BLOCK_SAMPLES * sizeof(float));
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "audio: pre-filled %u blocks start=0x%x ri_addr=0x%x",
                 nb, cfg.audioDataStart, cfg.readIndex);
        plog(buf);
    } else {
        plog("audio: pre-fill skipped (no dataStart)");
    }

    if (audioCreateNotifyEventQueue(&s_audio_eq, &s_audio_key) != 0) {
        audioPortClose(s_audio_port); audioQuit(); return;
    }
    audioSetNotifyEventQueue(s_audio_key);

    audioPortStart(s_audio_port);

    // Drain any spurious events that may have queued before Start completed.
    { sys_event_t ev; while (sysEventQueueReceive(s_audio_eq, &ev, 1) == 0) { } }

    s_audio_ok = true;
}

// Called once per displayed frame.  Drains the audio event queue with a 1 µs
// poll so the kernel queue never overflows.  Counts consumed blocks so callers
// can track elapsed audio time without additional syscalls.
// Must not call plog() — plog() flushes to HDD and stalls the PPU.
void audio_silence(void) {
    if (!s_audio_ok) return;
    sys_event_t ev;
    while (sysEventQueueReceive(s_audio_eq, &ev, 1) == 0) {
        if (s_read_index_addr && s_data_start) {
            // readIndex holds an ADDRESS to a u64 counter the SPU increments.
            // Read as u64* — on big-endian PS3, a u32* read would return the
            // high 32 bits (always 0 for block indices 0-7), giving blk=0 always.
            volatile u64 *ri_ptr  = (volatile u64 *)(uintptr_t)s_read_index_addr;
            u32           blk     = (u32)((*ri_ptr + 1) % s_num_blocks);
            float        *block   = (float *)(uintptr_t)(s_data_start +
                                    blk * 2 * AUDIO_BLOCK_SAMPLES * sizeof(float));
            memset(block, 0, 2 * AUDIO_BLOCK_SAMPLES * sizeof(float));
        }
        s_audio_blocks++;
    }
}

void audio_close(void) {
    if (!s_audio_ok) return;
    audioPortStop(s_audio_port);
    audioRemoveNotifyEventQueue(s_audio_key);
    audioPortClose(s_audio_port);
    sysEventQueueDestroy(s_audio_eq, 0);
    audioQuit();
    s_audio_ok        = false;
    s_read_index_addr = 0;
    s_data_start      = 0;
    s_num_blocks      = 0;
}
