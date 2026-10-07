#pragma once
// Display-mode vs content-cadence decisions, kept free of any PS3 header so the
// host suite can test them (tests/test_display_mode.c).
//
// WHAT THIS IS FOR.  Matching the HDMI output to 23.976 fps film -- a real
// 1080p23.976 signal instead of 3:2 pulldown on 59.94 -- needs three separate
// things, and only the first two exist:
//
//   A. know the content is 24000/1001        (vdec frc / server RealFrameRate)
//   B. present it correctly on 59.94          (Bresenham + duration gate, 3:2)
//   C. switch the PHYSICAL output to 24Hz     (display_24p.cpp)
//
// This module is the pure half: it turns what the firmware reports into a
// decision and a record, and classifies a MEASURED vblank period.  The
// switching itself is display_24p.cpp.  Full write-up: docs/24P_OUTPUT.md.
//
// WHY v1 cellVideoOutConfigure CANNOT DO IT, and Configure2 can -- read from
// the 4.93 firmware, not guessed:
//   * v1 sends VSH packet 0x4b07; VSH's handler (vsh.self 0x126e18) passes
//     refresh 0 = AUTO to its mode converter at every call site.
//   * cellVideoOutConfigure2 (cellSysutilAvconfExt) sends packet 0x4b08; VSH's
//     handler (0x126af8) reads a u16 refresh at config+0x0A and accepts exactly
//     {0, 0x01, 0x02, 0x10, 0x20, 0x40}.  That converter (0xc6b18) is the ONLY
//     place in VSH that honours a requested rate.
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Refresh-rate bits in videoDisplayMode.refreshRates / CellVideoOutDisplayMode.
// 0x01..0x08 are the SDK's values (RPCS3 cellVideoOut.h, PSL1GHT video.h).
#define DM_RATE_59_94   0x0001u
#define DM_RATE_50      0x0002u
#define DM_RATE_60      0x0004u
#define DM_RATE_30      0x0008u
// NOT in any public header.  Observed on hardware (videoGetDeviceInfo on the
// user's Panasonic UT30, 2026-09-21/22 logs):
//   res 1    (1920x1080)            rates 0x33 = 59.94 | 50 | 0x10 | 0x20
//   res 0x83 (1920x2205, 1080p FP)  rates 0x30 = 0x10 | 0x20 and nothing else
// HDMI 1.4 defines 1080p frame packing only at 23.98 and 24 Hz, so 0x10/0x20
// are the 24Hz family.  WHICH of the two is 23.976 and which is 24.000 is NOT
// established -- nothing here depends on it.
#define DM_RATE_24FAM_A 0x0010u
#define DM_RATE_24FAM_B 0x0020u
#define DM_RATE_24FAM   (DM_RATE_24FAM_A | DM_RATE_24FAM_B)
// res 0x82 (1920x1080) advertises ONLY this bit.  Meaning unknown.  It is the
// mode the 2026-09-18 experiment configured: the panel went black and the RSX
// stopped delivering flips.  It is not evidence of anything about 24Hz.
#define DM_RATE_UNK_40  0x0040u
#define DM_RATE_KNOWN   (DM_RATE_59_94 | DM_RATE_50 | DM_RATE_60 | DM_RATE_30)

// Resolution ids the decision cares about (SDK values).
#define DM_RES_1080     1u
#define DM_RES_720      2u

// Where the content frame rate came from, in falling order of trust.
typedef enum {
    DM_FPS_VDEC_FRC = 0,    // H.264 VUI frame-rate code decoded by VDEC
    DM_FPS_SERVER   = 1,    // Jellyfin RealFrameRate, snapped to a fraction
    DM_FPS_DEFAULT  = 2,    // no information: 30/1 guessed
    DM_FPS_TIMEOUT  = 3     // detection timed out: 30/1 guessed
} dm_fps_source;

typedef enum {
    DM_FILM_NONE   = 0,     // not 24fps-family content
    DM_FILM_23976  = 1,     // exactly 24000/1001
    DM_FILM_24     = 2      // exactly 24/1
} dm_film;

typedef enum {
    DM_SUPPORT_UNKNOWN = 0, // device info unreadable
    DM_SUPPORT_NO      = 1, // display lists the resolution without 24Hz bits
    DM_SUPPORT_YES     = 2  // firmware lists 24Hz-family bits for it (see above)
} dm_support;

typedef struct {
    uint8_t  res;           // resolution id
    uint16_t rates;         // refresh bitmask
} dm_mode;

// Map the current output's refresh bitmask to a rate.  Same precedence as
// timing_init() so the two can never disagree.  Returns 1 when a KNOWN rate
// was recognised, 0 when it fell back to the 59.94 default -- which is what
// timing_init() silently does with an unknown bit, so callers log it.
int dm_display_rate(uint16_t bits, uint32_t *num, uint32_t *den);

// Human-readable rate mask, e.g. "59.94|50|24fam(0x10)|24fam(0x20)".
void dm_rates_str(uint16_t bits, char *out, unsigned outsz);

dm_film dm_classify_film(uint32_t fps_num, uint32_t fps_den);

// A detected rate is "confident" only when it came from the stream or server;
// a guessed 30/1 is never a reason to touch the display.
int dm_fps_confident(dm_fps_source src);

const char *dm_fps_source_name(dm_fps_source src);

// Does the firmware report 24Hz-family support for resolution `res`?  Scans the
// device's advertised mode list; n < 0 means the list could not be read.
dm_support dm_display_24p_support(const dm_mode *modes, int n, uint8_t res);

// How content at fps_num/fps_den lands on a display at disp_num/disp_den,
// e.g. "1:1", "3:2 pulldown", "2:2", "uneven x2.400".  Pure arithmetic.
void dm_cadence_str(uint32_t disp_num, uint32_t disp_den,
                    uint32_t fps_num, uint32_t fps_den,
                    char *out, unsigned outsz);

typedef struct {
    int         candidate;     // content would benefit from 24Hz output
    const char *candidate_why; // short reason either way
    dm_support  support;
    const char *path;          // mechanism that performs the switch
    int         attempt;       // 1 only when every gate below passed
    const char *result;        // "not_attempted" or "pending"
    const char *result_why;
} dm_decision;

// Pure decision.  display_res/display_rates describe the CURRENT output;
// progressive is its scan mode; enabled is the user's opt-in.
dm_decision dm_decide(dm_film film, int fps_confident,
                      uint32_t width, uint32_t height,
                      uint8_t display_res, uint16_t display_rates,
                      int progressive, dm_support support, int enabled);

// What a measured vblank period says the output clock ACTUALLY is.  This is
// how 23.976 is told from 24.000: the two differ by 1000 ppm, and ~2 s of
// vblanks timed against the timebase resolves that with room to spare.
typedef enum {
    DM_CLK_UNKNOWN = 0,
    DM_CLK_23976, DM_CLK_24, DM_CLK_25, DM_CLK_2997, DM_CLK_30,
    DM_CLK_50, DM_CLK_5994, DM_CLK_60
} dm_clock;

// period_ns: mean vblank period.  Matches the nearest nominal rate within
// 250 ppm (a quarter of the 23.976/24 gap); anything else is UNKNOWN.
dm_clock dm_classify_period(uint64_t period_ns, uint32_t *num, uint32_t *den);
const char *dm_clock_name(dm_clock c);

const char *dm_support_name(dm_support s);

#ifdef __cplusplus
}
#endif
