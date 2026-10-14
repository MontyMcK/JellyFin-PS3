// The stream budget: what a quality step asks the server for.
//
// WHY THIS EXISTS (measured 2026-09-24, console log + server log together)
//
// The quality step "1080p 25 Mbps" sent VideoBitrate=25000000 -- the VIDEO
// rate.  With Surround HD on, the selected DTS-HD MA / TrueHD track is copied
// through untouched ON TOP of that, and MPEG-TS framing adds its own few
// percent.  The server's ffmpeg log for the failing sessions shows exactly
// that: -b:v 25000000 -maxrate 25000000, the DTS-HD MA 5.1 track copied, and
// an output that averaged 27.6 Mbps (peak 28.5).  This console receives at
// ~25 Mbps sustained (memory: ps3-playback-ceiling-findings), so the
// read-ahead ring drained at ~3 Mbps -- 186k -> 56k packets in a minute -- and
// the picture stalled once it emptied.  fps held 24.0 whenever data was there;
// the receive thread sat blocked in netRecv 70-99% of the time; ffmpeg ran at
// 16-18x realtime.  Nothing in the UI runs during video playback.
//
// So the step now means the WHOLE stream: the video is asked for at the step
// minus the audio that rides with it and the transport overhead.  A step with
// a plain AC-3/MP3 transcode loses well under a megabit to it; a lossless copy
// loses what the lossless track actually costs.  "Original" (0) still asks for
// no ceiling at all -- a copy of the source is its own budget.
//
// Pure C and host-tested (tests/test_stream_budget.c).

#ifndef JF_STREAM_BUDGET_H
#define JF_STREAM_BUDGET_H

#ifdef __cplusplus
extern "C" {
#endif

// MPEG-TS framing: 4 bytes of every 188, plus PES headers, PAT/PMT and PCR.
// Measured on the failing sessions as the gap between the requested rates and
// ffmpeg's muxed output once the audio is accounted for: ~3%.
#define STREAM_TS_OVERHEAD_PCT   3

// What a COPIED lossless track is assumed to cost when the server's own
// figure is missing or lower.  Jellyfin reports a DTS-HD MA track's CORE rate
// (1509 kbps) as its BitRate, which is not what arrives: the lossless
// extension rides in the same stream.  These are typical averages, on the
// high side on purpose -- over-reserving costs a little picture, under-
// reserving costs a stall.
#define STREAM_EST_DTS_HD_BPS    3000000u   // DTS-HD MA / DTS:X 5.1-7.1
#define STREAM_EST_TRUEHD_BPS    4500000u   // TrueHD / Atmos

// The video never drops below half the step, whatever the audio claims: a
// mislabelled or absurd track must not turn a 25 Mbps step into a 3 Mbps one.
#define STREAM_VIDEO_FLOOR_PCT  50

enum {
    STREAM_AUDIO_TRANSCODE = 0,   // the server encodes AC-3 / MP3 at audio_bps
    STREAM_AUDIO_COPY_DTS  = 1,   // a DTS-family track copied untouched
    STREAM_AUDIO_COPY_TRUEHD = 2  // a TrueHD track copied untouched
};

// What the audio is budgeted at.  reported_bps is the track's BitRate from
// PlaybackInfo (0 = unknown); for a transcode it is the requested rate.
static inline unsigned stream_audio_cost(int kind, unsigned reported_bps)
{
    unsigned est = 0;
    if (kind == STREAM_AUDIO_COPY_DTS)    est = STREAM_EST_DTS_HD_BPS;
    if (kind == STREAM_AUDIO_COPY_TRUEHD) est = STREAM_EST_TRUEHD_BPS;
    return reported_bps > est ? reported_bps : est;
}

// VideoBitrate to request for a quality step of step_bps (0 = Original: no
// ceiling, returned unchanged) carrying audio that costs audio_bps.
static inline unsigned stream_video_budget(unsigned step_bps, unsigned audio_bps)
{
    unsigned long long room, floor_bps;
    if (step_bps == 0) return 0;
    room      = (unsigned long long)step_bps * (100u - STREAM_TS_OVERHEAD_PCT) / 100u;
    floor_bps = (unsigned long long)step_bps * STREAM_VIDEO_FLOOR_PCT / 100u;
    room = room > audio_bps ? room - audio_bps : 0;
    if (room < floor_bps) room = floor_bps;
    return (unsigned)room;
}

#ifdef __cplusplus
}
#endif

#endif
