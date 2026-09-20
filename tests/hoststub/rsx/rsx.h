#pragma once
// Host stand-in for PSL1GHT's <rsx/rsx.h>.
//
// ui_draw.cpp (test_utf8) additionally reaches the clear path, which is why
// the clear entry points and GCM_CLEAR_* are below.
//
// ui_text.cpp uses the RSX only in drawChar()/drawTextScaled(), the 8x8 bitmap
// font fallback that runs when no TTF face loaded.  The TTF path under test
// never reaches it, so these are declarations and no-ops: enough to compile
// the real source unmodified, not enough to pretend anything was drawn.
#include <ppu-types.h>

typedef struct { int unused; } gcmContextData;

typedef struct {
    u32 conversion, format, origin, operation, interp;
    u32 clipX, clipY, clipW, clipH;
    u32 outX, outY, outW, outH;
    s32 ratioX, ratioY;
    u16 inX, inY, inW, inH;
    u32 offset, pitch;
} gcmTransferScale;

typedef struct { u32 format, pitch, offset; } gcmTransferSurface;

#define GCM_TRANSFER_CONVERSION_TRUNCATE    0
#define GCM_TRANSFER_SCALE_FORMAT_A8R8G8B8  0
#define GCM_TRANSFER_ORIGIN_CORNER          0
#define GCM_TRANSFER_OPERATION_SRCCOPY_AND  0
#define GCM_TRANSFER_INTERPOLATOR_NEAREST   0
#define GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8 0
#define GCM_TRANSFER_LOCAL_TO_LOCAL         0
#define GCM_TRANSFER_SURFACE                0

static inline s32 rsxGetFixedSint32(float f) { return (s32)(f * 65536.0f); }
static inline u16 rsxGetFixedUint16(int v)   { return (u16)v; }
static inline void rsxSetTransferScaleMode(gcmContextData *, u32, u32) {}
static inline void rsxSetTransferScaleSurface(gcmContextData *,
                                              gcmTransferScale *,
                                              gcmTransferSurface *) {}

// ---------------------------------------------------------------------------
// Surface clears.  ui_draw.cpp's clearScreen() calls these; the tests that
// compile it (test_utf8) never look at the cleared surface -- they draw text
// straight into color_buffer[] and read that back -- so no-ops are honest
// here in the same way the transfer stubs above are.
#define GCM_CLEAR_R  0x01
#define GCM_CLEAR_G  0x02
#define GCM_CLEAR_B  0x04
#define GCM_CLEAR_A  0x08
#define GCM_CLEAR_S  0x10
#define GCM_CLEAR_Z  0x20

static inline void rsxSetClearColor(gcmContextData *, u32) {}
static inline void rsxSetClearDepthStencil(gcmContextData *, u32) {}
static inline void rsxClearSurface(gcmContextData *, u32) {}
static inline void rsxSync(void) {}
