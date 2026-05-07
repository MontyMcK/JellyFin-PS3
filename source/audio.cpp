#include "audio.h"
#include "adec.h"
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
// sysEventQueueReceive success in audio_write_pcm().  Each block = 256 samples
// at 48 kHz = 5.333 ms.  Useful for A/V sync diagnostics.
static volatile u64 s_audio_blocks = 0;

u64 audio_block_count(void) { return s_audio_blocks; }

void audio_open(void) {
    int rc;
    char buf[128];

    rc = audioInit();
    snprintf(buf, sizeof(buf), "audio: sysAudioInit rc=0x%x", rc);
    plog(buf);
    if (rc != 0) return;

    audioPortParam p;
    p.numChannels = AUDIO_PORT_2CH;
    p.numBlocks   = AUDIO_BLOCK_8;
    p.attrib      = 0;
    p.level       = 1.0f;
    rc = audioPortOpen(&p, &s_audio_port);
    snprintf(buf, sizeof(buf), "audio: sysAudioPortOpen rc=0x%x port=%u", rc, s_audio_port);
    plog(buf);
    if (rc != 0) { audioQuit(); return; }

    // Per PSL1GHT docs the correct sequence is:
    //   Open → GetPortConfig → CreateEventQueue → SetEventQueue → Start
    // GetPortConfig must be called before Start; audioDataStart is valid
    // as soon as the port is opened.
    audioPortConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    rc = audioGetPortConfig(s_audio_port, &cfg);
    snprintf(buf, sizeof(buf), "audio: sysAudioGetPortConfig rc=0x%x", rc);
    plog(buf);
    snprintf(buf, sizeof(buf),
             "audio: cfg readIndex=0x%x status=0x%x",
             (unsigned)cfg.readIndex, (unsigned)cfg.status);
    plog(buf);
    snprintf(buf, sizeof(buf),
             "audio: cfg channelCount=0x%x portSize=0x%x portAddr=0x%x",
             (unsigned)cfg.channelCount, (unsigned)cfg.portSize,
             (unsigned)cfg.audioDataStart);
    plog(buf);

    if (rc == 0 && cfg.audioDataStart) {
        u32 nb = (u32)p.numBlocks;  // known value — don't trust cfg.numBlocks yet
        s_read_index_addr = cfg.readIndex;   // address of live SPU read-position u32
        s_data_start      = cfg.audioDataStart;
        s_num_blocks      = nb;
        // Zero the entire DMA ring.  Hardware reads zeros → digital silence.
        memset((void*)(uintptr_t)cfg.audioDataStart, 0,
               nb * 2 * AUDIO_BLOCK_SAMPLES * sizeof(float));
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
    rc = audioSetNotifyEventQueue(s_audio_key);
    snprintf(buf, sizeof(buf), "audio: audioSetNotifyEventQueue rc=0x%x", rc);
    plog(buf);

    rc = audioPortStart(s_audio_port);
    snprintf(buf, sizeof(buf), "audio: sysAudioPortStart rc=0x%x", rc);
    plog(buf);

    // Drain any spurious events that may have queued before Start completed.
    { sys_event_t ev; while (sysEventQueueReceive(s_audio_eq, &ev, 1) == 0) { } }

    s_audio_ok = true;
}

// Called from the dedicated audio thread.  Drains the audio event queue and
// fills each DMA block with decoded PCM from the adec ring, or silence.
void audio_write_pcm(void) {
    if (!s_audio_ok) return;
    static int s_clock_zero_count = 0;
    sys_event_t ev;
    while (sysEventQueueReceive(s_audio_eq, &ev, 1) == 0) {
        if (s_read_index_addr && s_data_start) {
            // readIndex holds an ADDRESS to a u32 block index the SPU increments.
            volatile u32 *ri_ptr = (volatile u32 *)(uintptr_t)s_read_index_addr;
            u32 clk = *ri_ptr;
            if (clk == 0) {
                s_clock_zero_count++;
                if (s_clock_zero_count > 2) {
                    char buf[64];
                    snprintf(buf, sizeof(buf),
                        "WARN audio: clock stalled %d frames", s_clock_zero_count);
                    plog(buf);
                }
            } else {
                s_clock_zero_count = 0;
            }
            u32  blk   = (clk + 1) % s_num_blocks;
            u32  addr  = s_data_start + blk * 2 * AUDIO_BLOCK_SAMPLES * sizeof(float);
            float *blk_buf = (float *)(uintptr_t)addr;
            float interleaved[AUDIO_BLOCK_SAMPLES * 2];
            int got = adec_read_pcm(interleaved, AUDIO_BLOCK_SAMPLES);
            // Deinterleave: PS3 expects all L samples then all R samples
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                blk_buf[i]                      = (i < got) ? interleaved[i * 2]     : 0.0f;  // L
                blk_buf[AUDIO_BLOCK_SAMPLES + i] = (i < got) ? interleaved[i * 2 + 1] : 0.0f;  // R
            }
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
