// The one compile of minimp3 the host tests link (the app has it in audio/adec.cpp).  The generic code, not SSE: the
// PS3 runs the generic code, so the host should too.
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
