#include "audio.h"
#include "adec.h"
#include "plog.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ppu-types.h>
#include <audio/audio.h>
#include <sys/event_queue.h>

static u32               s_audio_port  = 0;
static sys_event_queue_t s_audio_eq    = {0};
static sys_ipc_key_t     s_audio_key   = 0;
bool                     s_audio_ok    = false;
static u32               s_data_start  = 0;
static u32               s_num_blocks  = 0;
static u32               s_write_blk   = 0;  // next DMA block index to fill

// Total audio blocks consumed since port start.  Incremented once per
// sysEventQueueReceive success in audio_write_pcm().  Each block = 256 samples
// at 48 kHz = 5.333 ms.  Useful for A/V sync diagnostics.
static volatile u64 s_audio_blocks = 0;
static u32          s_pcm_blocks   = 0;
static u32          s_sil_blocks   = 0;

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
    snprintf(buf, sizeof(buf),
             "audio_param: ch=%llu blocks=%llu attrib=%llu level=%.4f",
             (unsigned long long)p.numChannels, (unsigned long long)p.numBlocks,
             (unsigned long long)p.attrib, (double)p.level);
    plog(buf);

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
             "audio: cfg channelCount=%llu numBlocks=%llu",
             (unsigned long long)cfg.channelCount, (unsigned long long)cfg.numBlocks);
    plog(buf);
    snprintf(buf, sizeof(buf),
             "audio: cfg portSize=0x%x portAddr=0x%x",
             (unsigned)cfg.portSize, (unsigned)cfg.audioDataStart);
    plog(buf);

    if (rc == 0 && cfg.audioDataStart) {
        u32 nb = (u32)p.numBlocks;  // known value — don't trust cfg.numBlocks yet
        s_data_start  = cfg.audioDataStart;
        s_num_blocks  = nb;
        s_write_blk   = 1;  // block 0 pre-filled with silence; hardware starts there
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

// Called from the dedicated audio thread in a loop — one block per call.
// Returns false immediately if no DMA event is ready (caller should sleep).
// After receiving an event, blocks until the ring has a full 256-sample block
// ready, sleeping 1ms per iteration up to a 100ms timeout.  On timeout writes
// silence and logs a stall diagnostic rather than playing partial-fill noise.
bool audio_write_pcm(void) {
    if (!s_audio_ok) return false;
    sys_event_t ev;
    if (sysEventQueueReceive(s_audio_eq, &ev, 0) != 0) return false;
    if (s_data_start) {
        u32   addr    = s_data_start + s_write_blk * 2 * AUDIO_BLOCK_SAMPLES * sizeof(float);
        float *blk_buf = (float *)(uintptr_t)addr;
        // Block until the decoder fills a complete block or the timeout fires.
        int waited = 0;
        while (adec_pcm_available() < AUDIO_BLOCK_SAMPLES && waited < 100) {
            usleep(1000);
            waited++;
        }
        if (adec_pcm_available() >= AUDIO_BLOCK_SAMPLES) {
            float interleaved[AUDIO_BLOCK_SAMPLES * 2];
            adec_read_pcm(interleaved, AUDIO_BLOCK_SAMPLES);
            // Deinterleave: PS3 expects all L samples then all R samples
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                blk_buf[i]                       = interleaved[i * 2];
                blk_buf[AUDIO_BLOCK_SAMPLES + i] = interleaved[i * 2 + 1];
            }
            s_pcm_blocks++;
        } else {
            // Decoder stall — write silence to keep DMA ring alive
            memset(blk_buf, 0, 2 * AUDIO_BLOCK_SAMPLES * sizeof(float));
            s_sil_blocks++;
            plog("audio: decoder stall");
        }
        s_write_blk = (s_write_blk + 1) % s_num_blocks;
    }
    u64 total = ++s_audio_blocks;
    if (total % 500 == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "audio_ratio: pcm=%u sil=%u total=%llu",
                 s_pcm_blocks, s_sil_blocks, (unsigned long long)total);
        plog(buf);
        s_pcm_blocks = 0;
        s_sil_blocks = 0;
    }
    return true;
}

void audio_close(void) {
    if (!s_audio_ok) return;
    audioPortStop(s_audio_port);
    audioRemoveNotifyEventQueue(s_audio_key);
    audioPortClose(s_audio_port);
    sysEventQueueDestroy(s_audio_eq, 0);
    audioQuit();
    s_audio_ok   = false;
    s_data_start = 0;
    s_num_blocks = 0;
    s_write_blk  = 0;
}
