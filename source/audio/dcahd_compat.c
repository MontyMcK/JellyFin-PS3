// DCA-only additions to the shared FFmpeg compat layer (mlp_compat.c already
// supplies av_malloc/av_log/av_channel_layout_*/ff_get_buffer and friends).
#include <stdlib.h>
#include <string.h>
#include "mlp/ff/libavcodec/avcodec.h"
#include "mlp/ff/libavutil/mem.h"

const uint8_t  ff_log2_tab[256] = {0};
const uint64_t ff_inverse[257]  = {0};

int av_channel_layout_custom_init(AVChannelLayout *ch, int nb) { (void)ch; (void)nb; return -1; }

void av_fast_malloc(void *ptr, unsigned int *size, size_t min_size)
{
    void **p = (void **)ptr;
    if (*p && *size >= min_size) return;
    av_free(*p);
    *p = av_malloc(min_size);
    *size = *p ? (unsigned int)min_size : 0;
}
void av_fast_mallocz(void *ptr, unsigned int *size, size_t min_size)
{
    void **p = (void **)ptr;
    if (*p && *size >= min_size) { memset(*p, 0, min_size); return; }
    av_free(*p);
    *p = av_mallocz(min_size);
    *size = *p ? (unsigned int)min_size : 0;
}

// --- deliberately absent subsystems --------------------------------------
// av_tx is FFmpeg MDCT/FFT framework, reached ONLY from the core decoder on
// its FLOAT output path.  DTS-HD MA decodes on the FIXED-POINT path, where
// the core synthesis filter is skipped entirely whenever an XLL substream is
// present.  Stubbing these keeps roughly 200 KB of transform code out of the
// build.  DTS Express (LBR) is lossy and never carries the lossless data.
int  av_tx_init(void **c, void **fn, int t, int inv, int len, const void *s, uint64_t f)
{ (void)c; (void)fn; (void)t; (void)inv; (void)len; (void)s; (void)f; return 0; }
void av_tx_uninit(void **c) { if (c) *c = 0; }
void ff_synth_filter_init(void *c) { (void)c; }
void ff_dca_lbr_init_tables(void) { }

int av_log2(unsigned v) { int n = 0; while (v >>= 1) n++; return n; }

// The float DSP context is reached only from the core float output path,
// which the fixed-point DTS-HD MA path never enters.  A zeroed context is
// enough for the pointer to be stored and never dereferenced.
void *avpriv_float_dsp_alloc(int bitexact)
{ (void)bitexact; return av_mallocz(512); }
