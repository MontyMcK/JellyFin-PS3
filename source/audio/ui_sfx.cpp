// XMB menu sound effects -- see ui_sfx.h.
//
// system_plugin.rco (PS3 firmware, big-endian RCO "FRP\0") keeps its sounds as
// plain VAG files, each with its 48-byte "VAGp" header and a name, laid end to
// end in the sound-data section.  Measured on this console's 4.9x flash:
//
//   SE02_Cursor  SE03_Normal_OK  SE04_Back  SE05_Category_OK  SE08_Option ...
//   mono, 48 kHz, 1104 bytes of PS-ADPCM = 1904 samples = 40 ms each
//
// (the RCO tree maps snd_cursor/snd_decide/snd_cancel/snd_option to them, but
// the tree is zlib-packed and the VAG names are enough, so no inflate here).
// The section's offset/length come from the RCO header; if those look wrong
// the whole file is scanned instead, since the VAG headers are self-describing.
//
// Output: a 2-channel port the effects thread keeps 3 blocks (16 ms) ahead of
// the DMA read index.  It polls rather than joining the audio notify queue so
// it shares nothing with audio.cpp.  Every block the hardware has finished
// with is zeroed straight away, so if this thread ever stalls the ring can
// only replay silence, never a stale click.

#include "ui_sfx.h"
#include "audio.h"
#include "plog.h"
#include "jf_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ppu-types.h>
#include <audio/audio.h>
#include <sys/thread.h>

#ifndef RCO_PATH
#define RCO_PATH   "/dev_flash/vsh/resource/system_plugin.rco"
#endif
#define SFX_FILE   "jellyfin_uisfx.txt"
#define SFX_BLOCKS 8      // ring size (AUDIO_BLOCK_8): 42 ms
#define SFX_AHEAD  3      // blocks kept queued ahead of the read index
#define SFX_VOICES 4

static const char *const kVagNames[SFX_COUNT] = {
    "SE02_Cursor", "SE03_Normal_OK", "SE04_Back", "SE08_Option",
};

static s16 *s_pcm[SFX_COUNT];
static int  s_len[SFX_COUNT];
static bool s_loaded    = false;
static bool s_enabled   = true;
static bool s_open      = false;   // port open and thread running

static u32               s_port    = 0;
static u32               s_data_ea = 0;
static u64               s_ri_ea   = 0;
static sys_ppu_thread_t  s_tid;
static volatile bool     s_run     = false;
static volatile u32      s_req[SFX_COUNT];   // bumped by ui_sfx_play

static u32 be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

// PS-ADPCM: 16-byte frames of (predictor<<4 | shift), flags, 28 nibbles.
static int vag_decode(const u8 *d, int n, s16 *out, int max) {
    static const int F[5][2] = { {0,0}, {60,0}, {115,-52}, {98,-55}, {122,-60} };
    int s1 = 0, s2 = 0, k = 0;
    for (int o = 0; o + 16 <= n; o += 16) {
        int flags = d[o + 1];
        if (flags == 7) break;                   // end marker frame
        int shift = d[o] & 15, pr = d[o] >> 4;
        if (pr > 4) pr = 4;
        for (int i = 0; i < 28 && k < max; i++) {
            int b   = d[o + 2 + i / 2];
            int nib = (i & 1) ? (b >> 4) : (b & 15);
            if (nib >= 8) nib -= 16;
            int v = (nib << 12) >> shift;
            v += (s1 * F[pr][0] + s2 * F[pr][1] + 32) >> 6;
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            s2 = s1; s1 = v;
            out[k++] = (s16)v;
        }
        if (flags & 1) break;                    // last frame of the sample
    }
    return k;
}

// Find each named VAG in buf and decode it.  Returns how many were found.
static int load_from(const u8 *buf, u32 n) {
    int found = 0;
    for (u32 i = 0; i + 48 <= n; i++) {
        if (buf[i] != 'V' || memcmp(buf + i, "VAGp", 4) != 0) continue;
        u32 size = be32(buf + i + 12), rate = be32(buf + i + 16);
        char name[17]; memcpy(name, buf + i + 32, 16); name[16] = '\0';
        if (rate != 48000 || size == 0 || size > n - i - 48) continue;
        for (int s = 0; s < SFX_COUNT; s++) {
            if (s_pcm[s] || strcmp(name, kVagNames[s]) != 0) continue;
            int max = (int)(size / 16) * 28;
            s16 *pcm = (s16 *)malloc((size_t)max * sizeof(s16));
            if (!pcm) break;
            s_len[s] = vag_decode(buf + i + 48, (int)size, pcm, max);
            s_pcm[s] = pcm;
            found++;
        }
        i += 47;
    }
    return found;
}

static bool load_sounds(void) {
    FILE *f = fopen(RCO_PATH, "rb");
    if (!f) { plog("uisfx: cannot open " RCO_PATH); return false; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    u8 hdr[0xA4];
    fseek(f, 0, SEEK_SET);
    bool hdr_ok = fsz > (long)sizeof(hdr) && fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr)
               && memcmp(hdr, "FRP\0", 4) == 0;
    // Sound-data offset and length sit at 0x88 / 0x8C in the PS3 RCO header.
    u32 off = hdr_ok ? be32(hdr + 0x88) : 0, len = hdr_ok ? be32(hdr + 0x8C) : 0;
    if (!hdr_ok || off >= (u32)fsz || len == 0 || len > (u32)fsz - off) {
        off = 0; len = (u32)fsz;                 // scan the whole file instead
    }
    u8 *buf = (u8 *)malloc(len);
    int found = 0;
    if (buf) {
        fseek(f, off, SEEK_SET);
        if (fread(buf, 1, len, f) == len) found = load_from(buf, len);
        free(buf);
    }
    fclose(f);
    char msg[128];
    snprintf(msg, sizeof msg, "uisfx: %d/%d sounds from rco (section 0x%x+0x%x)",
             found, SFX_COUNT, (unsigned)off, (unsigned)len);
    plog(msg);
    return found > 0;
}

// ---- effects thread ----

static inline float *block_ptr(u32 b) {
    return (float *)(uintptr_t)(s_data_ea + b * 2 * AUDIO_BLOCK_SAMPLES * sizeof(float));
}

static void sfx_thread(void *arg) {
    (void)arg;
    struct { int sfx, pos; } v[SFX_VOICES];
    for (int i = 0; i < SFX_VOICES; i++) v[i].sfx = -1;
    u32 seen[SFX_COUNT];
    for (int s = 0; s < SFX_COUNT; s++) seen[s] = s_req[s];

    u32 last = (u32)(*(volatile u64 *)(uintptr_t)s_ri_ea) % SFX_BLOCKS;
    u32 wr   = (last + 1) % SFX_BLOCKS;
    while (s_run) {
        u32 ri = (u32)(*(volatile u64 *)(uintptr_t)s_ri_ea) % SFX_BLOCKS;
        bool behind = false;
        for (u32 b = last; b != ri; b = (b + 1) % SFX_BLOCKS) {
            memset(block_ptr(b), 0, 2 * AUDIO_BLOCK_SAMPLES * sizeof(float));
            if (b == wr) behind = true;
        }
        last = ri;
        if (behind || wr == ri) wr = (ri + 1) % SFX_BLOCKS;

        // New requests start a voice (stealing the furthest-along one if full).
        for (int s = 0; s < SFX_COUNT; s++) {
            u32 r = s_req[s];
            if (r == seen[s]) continue;
            seen[s] = r;
            if (!s_pcm[s]) continue;
            int pick = 0;
            for (int i = 0; i < SFX_VOICES; i++) {
                if (v[i].sfx < 0) { pick = i; break; }
                if (v[i].pos > v[pick].pos) pick = i;
            }
            v[pick].sfx = s; v[pick].pos = 0;
        }

        while ((wr + SFX_BLOCKS - ri) % SFX_BLOCKS <= SFX_AHEAD) {
            float *out = block_ptr(wr);
            memset(out, 0, 2 * AUDIO_BLOCK_SAMPLES * sizeof(float));
            for (int i = 0; i < SFX_VOICES; i++) {
                if (v[i].sfx < 0) continue;
                const s16 *pcm = s_pcm[v[i].sfx];
                int n = s_len[v[i].sfx] - v[i].pos;
                if (n > AUDIO_BLOCK_SAMPLES) n = AUDIO_BLOCK_SAMPLES;
                for (int k = 0; k < n; k++) {
                    float x = pcm[v[i].pos + k] * (1.0f / 32768.0f);
                    out[2 * k] += x; out[2 * k + 1] += x;
                }
                v[i].pos += n;
                if (v[i].pos >= s_len[v[i].sfx]) v[i].sfx = -1;
            }
            wr = (wr + 1) % SFX_BLOCKS;
        }
        usleep(2000);
    }
    sysThreadExit(0);
}

static bool port_open(void) {
    if (audio_sys_acquire() != 0) { plog("uisfx: audioInit failed"); return false; }
    audioPortParam p;
    p.numChannels = AUDIO_PORT_2CH;
    p.numBlocks   = AUDIO_BLOCK_8;
    p.attrib      = 0;
    p.level       = 1.0f;
    audioPortConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    int rc = audioPortOpen(&p, &s_port);
    if (rc == 0) rc = audioGetPortConfig(s_port, &cfg);
    if (rc != 0 || !cfg.audioDataStart || !cfg.readIndex) {
        char msg[64];
        snprintf(msg, sizeof msg, "uisfx: port open failed rc=0x%x", rc);
        plog(msg);
        if (rc == 0) audioPortClose(s_port);
        audio_sys_release();
        return false;
    }
    s_data_ea = cfg.audioDataStart;
    s_ri_ea   = (u64)cfg.readIndex;
    memset((void *)(uintptr_t)s_data_ea, 0,
           SFX_BLOCKS * 2 * AUDIO_BLOCK_SAMPLES * sizeof(float));
    audioPortStart(s_port);
    s_run = true;
    // Above the UI thread's priority (lower number) so a busy frame cannot
    // starve it, and it sleeps almost all of the time.
    if (sysThreadCreate(&s_tid, sfx_thread, NULL, 900, 0x4000,
                        THREAD_JOINABLE, (char *)"jf_uisfx") != 0) {
        s_run = false;
        audioPortStop(s_port);
        audioPortClose(s_port);
        audio_sys_release();
        plog("uisfx: thread create failed");
        return false;
    }
    return true;
}

static void port_close(void) {
    s_run = false;
    u64 ret;
    sysThreadJoin(s_tid, &ret);
    audioPortStop(s_port);
    audioPortClose(s_port);
    audio_sys_release();
}

void ui_sfx_init(void) {
    FILE *f = fopen(jf_data_path(SFX_FILE), "r");
    if (f) {
        int on = 1;
        if (fscanf(f, "%d", &on) == 1) s_enabled = (on != 0);
        fclose(f);
    }
    if (!s_enabled) { plog("uisfx: off (jellyfin_uisfx.txt)"); return; }
    s_loaded = load_sounds();
    if (s_loaded) s_open = port_open();
    plog(s_open ? "uisfx: port open" : "uisfx: disabled");
}

void ui_sfx_play(int sfx) {
    if (!s_open || sfx < 0 || sfx >= SFX_COUNT) return;
    s_req[sfx]++;
}

void ui_sfx_suspend(void) {
    if (!s_open) return;
    port_close();
    s_open = false;
}

void ui_sfx_resume(void) {
    if (s_open || !s_loaded || !s_enabled) return;
    s_open = port_open();
}
