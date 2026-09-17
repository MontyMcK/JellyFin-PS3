#include "audio.h"
#include "adec.h"
#include "plog.h"
#include "jf_paths.h"
#include "player_stats.h"
#include "centermix.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ppu-types.h>
#include <audio/audio.h>
#include <sys/event_queue.h>

extern void crash_log(const char *msg);

static u32               s_audio_port  = 0;
static sys_event_queue_t s_audio_eq    = {0};
static sys_ipc_key_t     s_audio_key   = 0;
bool                     s_audio_ok    = false;
static u32               s_data_start  = 0;
static u32               s_num_blocks  = 0;
static u32               s_write_blk   = 0;  // next DMA block index to fill
// CellAudio port width (2 or 8 — PSL1GHT audio/audio.h defines only
// AUDIO_PORT_2CH and AUDIO_PORT_8CH) and the meaningful program channels
// carried in it (2 or 6).  A 5.1 program rides an 8-wide port with the two
// rear slots zeroed every block.
static int               s_port_channels   = 2;
static int               s_output_channels = 2;
// EA of the hardware read index (audioPortConfig.readIndex): a u64 holding the
// block the DMA engine is currently playing.  Retained purely so the stats
// overlay can report how many blocks of runway sit ahead of the read cursor —
// nothing in the playback path reads it.
static u64               s_read_idx_ea = 0;

// Total audio blocks consumed since port start.  Incremented once per
// sysEventQueueReceive success in audio_write_pcm().  Each block = 256 samples
// at 48 kHz = 5.333 ms.  Useful for A/V sync diagnostics.
static volatile u64 s_audio_blocks = 0;
static u32          s_pcm_blocks   = 0;
static u32          s_sil_blocks   = 0;

u64 audio_block_count(void) { return s_audio_blocks; }

// ---- PCM source (defaults to the video pipeline's decoder) ----
static audio_avail_fn    s_src_avail    = adec_pcm_available;
static audio_read_fn     s_src_read     = adec_read_pcm;
static audio_channels_fn s_src_channels = adec_output_channels;

void audio_set_source(audio_avail_fn avail, audio_read_fn read,
                      audio_channels_fn channels) {
    s_src_avail    = avail    ? avail    : adec_pcm_available;
    s_src_read     = read     ? read     : adec_read_pcm;
    s_src_channels = channels ? channels : adec_output_channels;
}

int audio_output_channels(void) { return s_output_channels; }

// ---- Master volume (0..100 %) ----
// Applied as a linear gain to the float PCM block just before it is DMA'd to
// the audio port.  100 % = unity (no scaling, bit-exact).  Persisted so the
// level is remembered across sessions.  Adjusted from the player HUD's volume
// slider (d-pad up/down on the speaker control).
#define VOLUME_FILE "jellyfin_volume.txt"
static volatile int s_volume_pct = 100;

int  audio_get_volume(void) { return s_volume_pct; }

void audio_set_volume(int pct) {
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    if (pct == s_volume_pct) return;
    s_volume_pct = pct;
    FILE *f = fopen(jf_data_path(VOLUME_FILE), "w");
    if (f) { fprintf(f, "%d\n", pct); fclose(f); }
}

void audio_volume_load(void) {
    FILE *f = fopen(jf_data_path(VOLUME_FILE), "r");
    if (!f) return;
    int pct = 100;
    if (fscanf(f, "%d", &pct) == 1) {
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        s_volume_pct = pct;
    }
    fclose(f);
}

// Scale one interleaved float block in place by the master volume.
static void apply_volume(float *buf, int frames, int channels) {
    int pct = s_volume_pct;
    if (pct >= 100) return;              // unity: leave the block untouched
    float g = (float)pct / 100.0f;
    int n = frames * channels;
    for (int i = 0; i < n; i++) buf[i] *= g;
}

u64 audio_get_clock_us(void) {
    u64 read_pts = adec_get_read_pts_us();
    if (read_pts == 0)
        return (s_audio_blocks * 256ULL * 1000000ULL) / 48000ULL;
    // adec_get_read_pts_us() is the PTS of the next sample entering the hardware
    // DMA pipeline.  The sample currently audible is (s_num_blocks - 1) blocks
    // behind that point.
    const u64 hw_latency_us =
        ((u64)(s_num_blocks - 1) * AUDIO_BLOCK_SAMPLES * 1000000ULL)
        / 48000ULL;
    return (read_pts > hw_latency_us) ? (read_pts - hw_latency_us) : 0;
}

bool audio_clock_valid(void) {
    return adec_get_read_pts_us() != 0;
}

void audio_open(int channels) {
    crash_log("a1 audio_open enter");
    int rc;
    char buf[128];

    crash_log("a2 sysAudioInit");
    rc = audioInit();
    snprintf(buf, sizeof(buf), "audio: sysAudioInit rc=0x%x", rc);
    plog(buf);
    if (rc != 0) return;

    // 8-wide surround port gets 16 blocks: Movian's ps3_audio.c uses 16, and
    // 8 blocks is only ~42 ms of runway — thin once the decoder is doing real
    // 5.1 work.  The stereo path keeps the shipped 8 blocks untouched.
    // Port width.  A 6-channel port was tried here, on the theory that a 5.1
    // program in an 8-wide port makes the firmware convert 8->6 and that the
    // centre could go missing in that conversion.  THE PS3 REJECTS IT:
    // hardware logged `6ch port open rc=0x80310704`, then opened 8ch fine.
    // PSL1GHT defines only 2CH and 8CH and it turns out that is not an
    // oversight. Do not try this again; the centre channel is lost somewhere
    // downstream of the console, not in a width conversion.
    //
    // The loop is kept because the fallback through to stereo is still what
    // makes a failed surround open degrade instead of killing playback.
    bool surround = (channels == 8);
    audioPortParam p;
    p.numBlocks = surround ? AUDIO_BLOCK_16 : AUDIO_BLOCK_8;
    p.attrib    = 0;
    p.level     = 1.0f;
    crash_log("a3 sysAudioPortOpen");

    static const u64 kWidths[] = { AUDIO_PORT_8CH };
    int opened = 0;
    if (surround) {
        for (unsigned i = 0; i < sizeof(kWidths) / sizeof(kWidths[0]); i++) {
            p.numChannels = kWidths[i];
            rc = audioPortOpen(&p, &s_audio_port);
            snprintf(buf, sizeof(buf), "audio: %uch port open rc=0x%x",
                     (unsigned)kWidths[i], rc);
            plog(buf);
            if (rc == 0) { opened = (int)kWidths[i]; break; }
        }
    }
    if (!opened) {
        // Every surround width rejected (or stereo asked for): fall back to
        // the shipped stereo port rather than failing playback outright.
        if (surround)
            plog("audio: no surround port available, falling back to 2ch");
        surround      = false;
        p.numChannels = AUDIO_PORT_2CH;
        p.numBlocks   = AUDIO_BLOCK_8;
        rc = audioPortOpen(&p, &s_audio_port);
        opened = 2;
    }
    s_port_channels   = opened;
    s_output_channels = opened;   // program capacity, not a fixed 5.1
    snprintf(buf, sizeof(buf), "audio: sysAudioPortOpen rc=0x%x port=%u", rc, s_audio_port);
    plog(buf);
    if (rc != 0) { audioQuit(); return; }
    snprintf(buf, sizeof(buf), "audio_open: ch=%d blocks=%d",
             s_port_channels, (int)p.numBlocks);
    plog(buf);
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
        s_read_idx_ea = (u64)cfg.readIndex;
        // Zero the entire DMA ring.  Hardware reads zeros → digital silence.
        memset((void*)(uintptr_t)cfg.audioDataStart, 0,
               nb * s_port_channels * AUDIO_BLOCK_SAMPLES * sizeof(float));
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

    crash_log("a4 sysAudioPortStart");
    rc = audioPortStart(s_audio_port);
    snprintf(buf, sizeof(buf), "audio: sysAudioPortStart rc=0x%x", rc);
    plog(buf);

    // Drain any spurious events that may have queued before Start completed.
    { sys_event_t ev; while (sysEventQueueReceive(s_audio_eq, &ev, 1) == 0) { } }

    s_audio_ok = true;
    crash_log("a5 audio_open done");
}

// Called from the dedicated audio thread in a loop — one block per call.
// Returns false immediately if no DMA event is ready (caller should sleep).
// After receiving an event, blocks until the ring has a full 256-sample block
// ready, sleeping 1ms per iteration up to a 30ms timeout.  On timeout writes
// silence and logs a stall diagnostic rather than playing partial-fill noise.
bool audio_write_pcm(void) {
    if (!s_audio_ok) return false;
    sys_event_t ev;
    if (sysEventQueueReceive(s_audio_eq, &ev, 0) != 0) return false;
    if (s_data_start) {
        // PSL1GHT audio/audio.h: block address = audioDataStart +
        // blk * numChannels * AUDIO_BLOCK_SAMPLES * sizeof(float).
        // numChannels is the PORT width (8 for surround), and a block is
        // always 256 sample FRAMES regardless of width.
        u32   addr    = s_data_start + s_write_blk * s_port_channels
                        * AUDIO_BLOCK_SAMPLES * sizeof(float);
        float *blk_buf = (float *)(uintptr_t)addr;
        // Block until the decoder fills a complete block or the timeout fires.
        int waited = 0;
        while (s_src_avail() < AUDIO_BLOCK_SAMPLES && waited < 30) {
            usleep(1000);
            waited++;
        }
        int  avail   = s_src_avail();
        bool starved = (avail < AUDIO_BLOCK_SAMPLES);
        if (!starved) {
            int src_ch = s_src_channels();
            if (s_port_channels == 2 && src_ch == 2) {
                // Shipped stereo path — source reads straight into the block.
                s_src_read(blk_buf, AUDIO_BLOCK_SAMPLES);
                apply_volume(blk_buf, AUDIO_BLOCK_SAMPLES, 2);
            } else {
                // Source width != port width: stage the source frames, then
                // place each one inside the wider port frame.  Static — this
                // is the audio thread's hot path, no malloc and no big stack
                // (thread stacks here are small; see hard constraints).
                static float stage[AUDIO_BLOCK_SAMPLES * 8];
                if (src_ch < 1 || src_ch > 8) src_ch = 2;   // defensive clamp
                s_src_read(stage, AUDIO_BLOCK_SAMPLES);
                apply_volume(stage, AUDIO_BLOCK_SAMPLES, src_ch);
                // Dialogue handling (centre boost / phantom fold).  Done here,
                // on the staged source frame, so it covers every codec at once
                // and cannot disagree with the per-codec channel maps.
                centermix_apply(stage, AUDIO_BLOCK_SAMPLES, src_ch);
                for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                    float       *d = blk_buf + i * s_port_channels;
                    const float *s = stage   + i * src_ch;
                    // A 6ch source is already in PS3 order (FL FR FC LFE SL
                    // SR — see channel map derivation in adec_ac3.cpp), so it
                    // maps 1:1 onto the first six port slots; an 8ch source
                    // (TrueHD 7.1, truehd_map.c) fills all eight the same
                    // way.  A 2ch source in an 8-wide port fills FL/FR only.
                    //
                    // The layouts are prefix-compatible (FL FR FC LFE SL SR
                    // [BL BR]), so a source WIDER than the port keeps as many
                    // leading channels as fit rather than collapsing to FL/FR.
                    // That matters: collapsing would silently drop the centre,
                    // taking the dialogue with it, which is the loudest
                    // possible failure for the quietest possible reason.
                    int copy = (src_ch <= s_port_channels) ? src_ch
                                                           : s_port_channels;
                    int c = 0;
                    for (; c < copy; c++)            d[c] = s[c];
                    // Zero every unused slot INCLUDING rears 6/7 each block —
                    // stale data in port memory is audible on those speakers.
                    for (; c < s_port_channels; c++) d[c] = 0.0f;
                }
            }
            s_pcm_blocks++;
        } else {
            // Decoder stall — write silence to keep DMA ring alive
            memset(blk_buf, 0, s_port_channels * AUDIO_BLOCK_SAMPLES * sizeof(float));
            s_sil_blocks++;
            plog("audio: decoder stall");
        }
        // Per-channel peak of the block we just handed to the DMA engine —
        // the last point the app can observe its own audio.  This is what
        // answers "is the centre channel silent, or is the chain not playing
        // it?": a centre peak tracking dialogue with nothing audible means
        // the sink is dropping slot 2, not that the decode is wrong.
        {
            int pk[8] = {0,0,0,0,0,0,0,0};
            const int pc = (s_port_channels > 8) ? 8 : s_port_channels;
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                const float *s = blk_buf + i * s_port_channels;
                for (int c = 0; c < pc; c++) {
                    float a = s[c] < 0.0f ? -s[c] : s[c];
                    int   q = (int)(a * 32768.0f);
                    if (q > pk[c]) pk[c] = q;
                }
            }
            player_stats_on_audio_levels(pk, pc);
        }

        s_write_blk = (s_write_blk + 1) % s_num_blocks;

        // Blocks of runway between the DMA read cursor and where we just
        // wrote — the ring's actual safety margin.
        {
            int ahead = 0;
            if (s_read_idx_ea && s_num_blocks) {
                u32 rd = (u32)(*(volatile u64 *)(uintptr_t)s_read_idx_ea);
                rd %= s_num_blocks;
                ahead = (int)((s_write_blk + s_num_blocks - rd) % s_num_blocks);
            }
            player_stats_on_audio_write(avail, ahead, (int)s_num_blocks, starved);
        }
    }
    u64 total = ++s_audio_blocks;
    if (total % 500 == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "audio_ratio: pcm=%u sil=%u total=%llu",
                 s_pcm_blocks, s_sil_blocks, (unsigned long long)total);
        plog(buf);
        s_pcm_blocks = 0;
        s_sil_blocks = 0;
        u64 clk = audio_get_clock_us();
        snprintf(buf, sizeof(buf), "audio_clock: clk=%lluus blocks=%llu",
                 (unsigned long long)clk, (unsigned long long)total);
        plog(buf);
    }
    return true;
}

void audio_close(void) {
    crash_log("ax1 audio_close enter");
    if (!s_audio_ok) return;
    crash_log("ax2 sysAudioPortStop");
    audioPortStop(s_audio_port);
    audioRemoveNotifyEventQueue(s_audio_key);
    crash_log("ax3 sysAudioPortClose");
    audioPortClose(s_audio_port);
    sysEventQueueDestroy(s_audio_eq, 0);
    crash_log("ax4 sysAudioQuit");
    audioQuit();
    s_audio_ok    = false;
    s_data_start  = 0;
    s_num_blocks  = 0;
    s_write_blk   = 0;
    s_read_idx_ea = 0;
    s_port_channels   = 2;
    s_output_channels = 2;
    crash_log("ax5 audio_close done");
}
