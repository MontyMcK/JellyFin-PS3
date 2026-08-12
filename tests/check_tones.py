#!/usr/bin/env python3
"""Goertzel per-channel tone check for interleaved f32 6ch PCM at 48 kHz.
Used to verify what ffmpeg's own decoder says each channel of tones51.ac3
carries (ground truth for the encoder-side channel assignment).
Usage: check_tones.py decoded.f32 [channels]
"""
import struct, math, sys

path = sys.argv[1]
nch = int(sys.argv[2]) if len(sys.argv) > 2 else 6
data = open(path, "rb").read()
n = len(data) // 4
smp = struct.unpack("<%df" % n, data)
frames = n // nch
freqs = [300, 500, 700, 60, 900, 1100]
for ch in range(nch):
    powers = []
    for f in freqs:
        w = 2 * math.pi * f / 48000.0
        cw = 2 * math.cos(w)
        s1 = s2 = 0.0
        for i in range(frames):
            s0 = smp[i * nch + ch] + cw * s1 - s2
            s2 = s1
            s1 = s0
        powers.append((f, s1 * s1 + s2 * s2 - cw * s1 * s2))
    powers.sort(key=lambda t: -t[1])
    print("ch%d dominant: %4d Hz   (2nd: %4d Hz, ratio %.1f dB)" % (
        ch, powers[0][0], powers[1][0],
        10 * math.log10(powers[0][1] / max(powers[1][1], 1e-30))))
